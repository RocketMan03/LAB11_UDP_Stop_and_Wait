
// UDPClientThdDlg.cpp

#include "pch.h"
#include "framework.h"
#include "UDPClientThd.h"
#include "UDPClientThdDlg.h"
#include "CDataSocket.h"
#include "afxdialogex.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

CCriticalSection rx_cs, tx_cs;


// 프로그램 정보 표시용 About 다이얼로그
class CAboutDlg : public CDialogEx
{
public:
	CAboutDlg();

#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_ABOUTBOX };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);

protected:
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialogEx(IDD_ABOUTBOX)
{
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialogEx)
END_MESSAGE_MAP()


static unsigned short onesCompSum(const void* data, int bytes)
{
	unsigned long sum = 0;
	const unsigned short* p = (const unsigned short*)data;
	while (bytes > 1) {
		sum += *p++;
		bytes -= 2;
	}
	if (bytes == 1)
		sum += *(const unsigned char*)p;
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);
	return (unsigned short)sum;
}

int getChecksum(Frame frame)
{
	frame.checksum = 0;
	return (int)(unsigned short)(~onesCompSum(&frame, sizeof(Frame)));
}


CUDPClientThdDlg::CUDPClientThdDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_UDPCLIENTTHD_DIALOG, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	m_pDataSocket = NULL;
	m_hAckEvent = CreateEvent(NULL, FALSE, FALSE, NULL); // auto-reset
	m_ackSeqNum = -1;
	m_ackType = 0;
	m_expectedSeq = 0;
	m_pendingAckSeq = -1;
	m_pendingAckTime = 0;
}

void CUDPClientThdDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_IPADDRESS1, m_ipaddr);
	DDX_Control(pDX, IDC_EDIT1, m_tx_edit_short);
	DDX_Control(pDX, IDC_EDIT2, m_tx_edit);
	DDX_Control(pDX, IDC_EDIT3, m_rx_edit);
	DDX_Control(pDX, IDC_EDIT4, m_CheckSum_tx);
	DDX_Control(pDX, IDC_EDIT5, m_CheckSum_rx);
}

BEGIN_MESSAGE_MAP(CUDPClientThdDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_BN_CLICKED(IDC_BUTTON1, &CUDPClientThdDlg::OnBnClickedButton1)
	ON_BN_CLICKED(IDC_BUTTON2, &CUDPClientThdDlg::OnBnClickedButton2)
END_MESSAGE_MAP()


// RX 큐에 쌓인 문자열을 꺼내 수신 에디트박스에 표시하는 스레드
// 피기배킹 타임아웃(200ms) 초과 시 보류 ACK를 순수 ACK로 즉시 전송
UINT RXThread(LPVOID arg)
{
	const int PIGGY_TIMEOUT_MS = 1000;

	ThreadArg* pArg = (ThreadArg*)arg;
	CStringList* plist = pArg->pList;
	CUDPClientThdDlg* pDlg = (CUDPClientThdDlg*)pArg->pDlg;
	while (pArg->Thread_run) {
		POSITION pos = plist->GetHeadPosition();
		POSITION current_pos;
		while (pos != NULL) {
			current_pos = pos;
			rx_cs.Lock();
			CString str = plist->GetNext(pos);
			rx_cs.Unlock();

			int len = pDlg->m_rx_edit.GetWindowTextLengthW();
			pDlg->m_rx_edit.SetSel(len, len);
			pDlg->m_rx_edit.ReplaceSel(str);

			plist->RemoveAt(current_pos);
		}

		// 보류 중인 ACK가 타임아웃되면 순수 ACK 프레임으로 전송
		int pendingSeq = pDlg->m_pendingAckSeq;
		if (pendingSeq != -1 &&
			GetTickCount() - pDlg->m_pendingAckTime > PIGGY_TIMEOUT_MS)
		{
			if (pDlg->m_pDataSocket != NULL && !pDlg->m_serverIP.IsEmpty()) {
				Frame ackFrame;
				ackFrame.frame_type = 1; // ACK
				ackFrame.ack_num    = pendingSeq;
				ackFrame.checksum   = getChecksum(ackFrame);
				pDlg->m_pendingAckSeq = -1;
				pDlg->m_pDataSocket->SendTo(&ackFrame, sizeof(Frame), 8000, pDlg->m_serverIP);

				CString strLog;
				strLog.Format(_T("STANDALONE-ACK ack_num=%d\r\n"), pendingSeq);
				int csLen = pDlg->m_CheckSum_rx.GetWindowTextLengthW();
				pDlg->m_CheckSum_rx.SetSel(csLen, csLen);
				pDlg->m_CheckSum_rx.ReplaceSel(strLog);

				csLen = pDlg->m_CheckSum_rx.GetWindowTextLengthW();
				pDlg->m_CheckSum_rx.SetSel(csLen, csLen);
				pDlg->m_CheckSum_rx.ReplaceSel(_T("---------------------------\r\n"));
			}
		}

		Sleep(10);
	}
	return 0;
}

// TX 스레드: Stop-and-Wait, 프레임 전송 후 ACK 대기, 타임아웃/NAK 시 재전송
UINT TXThread(LPVOID arg)
{
	const int TIMEOUT_MS = 2000; // ACK 대기 타임아웃 (ms)
	const int MAX_RETRY  = 5;    // 최대 재전송 횟수

	Frame frame;

	ThreadArg* pArg = (ThreadArg*)arg;
	CStringList* plist = pArg->pList;
	CUDPClientThdDlg* pDlg = (CUDPClientThdDlg*)pArg->pDlg;

	while (pArg->Thread_run) {
		POSITION pos = plist->GetHeadPosition();
		POSITION current_pos;
		while (pos != NULL) {
			current_pos = pos;
			tx_cs.Lock();
			CString str = plist->GetNext(pos);
			tx_cs.Unlock();

			if (pDlg->m_pDataSocket != NULL && !pDlg->m_serverIP.IsEmpty())
			{
				const int chunkLen = _countof(frame.p_buffer) - 1;
				int offset = 0;
				int totalLen = str.GetLength();
				int frameNum = 0;
				bool msgFailed = false;
				DWORD startTime = GetTickCount();

				do {
					if (!pArg->Thread_run) { msgFailed = true; break; }

					CString chunk = str.Mid(offset, chunkLen);
					int retries = 0;
					bool ackOk  = false;

					while (!ackOk && retries <= MAX_RETRY && pArg->Thread_run) {
						// DATA 프레임 구성
						_tcscpy_s(frame.p_buffer, _countof(frame.p_buffer), chunk);
						frame.frame_type    = 0; // DATA
						frame.Frame_num     = frameNum;
						frame.is_last_Frame = (offset + chunkLen >= totalLen) ? 1 : 0;

						// 보류 중인 ACK가 있으면 피기배킹
						frame.has_piggybacked_ack = 0;
						frame.ack_num = 0;
						int pendingAck = pDlg->m_pendingAckSeq;
						if (pendingAck != -1) {
							frame.has_piggybacked_ack = 1;
							frame.ack_num = pendingAck;
							pDlg->m_pendingAckSeq = -1;

							int rxLen = pDlg->m_CheckSum_rx.GetWindowTextLengthW();
							pDlg->m_CheckSum_rx.SetSel(rxLen, rxLen);
							pDlg->m_CheckSum_rx.ReplaceSel(_T("---------------------------\r\n"));
						}

						frame.checksum = getChecksum(frame);

						// 이전 ACK 신호 초기화 후 전송
						ResetEvent(pDlg->m_hAckEvent);
						pDlg->m_pDataSocket->SendTo(&frame, sizeof(Frame), 8000, pDlg->m_serverIP);

						// TX 로그
						CString strLog;
						if (retries == 0) {
							if (frame.has_piggybacked_ack)
								strLog.Format(_T("TX  seq=%d frame=%d%s cs=0x%04X [PIGGY ack=%d]\r\n"),
									frame.seq_num, frame.Frame_num,
									frame.is_last_Frame ? _T("[L]") : _T(""),
									(unsigned short)frame.checksum, frame.ack_num);
							else
								strLog.Format(_T("TX  seq=%d frame=%d%s cs=0x%04X\r\n"),
									frame.seq_num, frame.Frame_num,
									frame.is_last_Frame ? _T("[L]") : _T(""),
									(unsigned short)frame.checksum);
						}
						else
							strLog.Format(_T("RETRY#%d seq=%d frame=%d cs=0x%04X\r\n"),
								retries, frame.seq_num, frame.Frame_num,
								(unsigned short)frame.checksum);
						int csLen = pDlg->m_CheckSum_tx.GetWindowTextLengthW();
						pDlg->m_CheckSum_tx.SetSel(csLen, csLen);
						pDlg->m_CheckSum_tx.ReplaceSel(strLog);

						// ACK/NAK 대기
						DWORD wr = WaitForSingleObject(pDlg->m_hAckEvent, TIMEOUT_MS);

						if (wr == WAIT_OBJECT_0
							&& pDlg->m_ackSeqNum == frame.seq_num
							&& pDlg->m_ackType   == 1)
						{
							ackOk = true;
							CString strAck;
							strAck.Format(_T("[ACK] seq=%d OK\r\n"), frame.seq_num);
							csLen = pDlg->m_CheckSum_tx.GetWindowTextLengthW();
							pDlg->m_CheckSum_tx.SetSel(csLen, csLen);
							pDlg->m_CheckSum_tx.ReplaceSel(strAck);
						}
						else {
							retries++;
							CString strRe;
							if (wr == WAIT_TIMEOUT)
								strRe.Format(_T("TIMEOUT seq=%d retry=%d/%d\r\n"),
									frame.seq_num, retries, MAX_RETRY);
							else if (pDlg->m_ackType == 2)
								strRe.Format(_T("NAK seq=%d retry=%d/%d\r\n"),
									frame.seq_num, retries, MAX_RETRY);
							else
								strRe.Format(_T("WRONG_ACK seq=%d retry=%d/%d\r\n"),
									frame.seq_num, retries, MAX_RETRY);
							csLen = pDlg->m_CheckSum_tx.GetWindowTextLengthW();
							pDlg->m_CheckSum_tx.SetSel(csLen, csLen);
							pDlg->m_CheckSum_tx.ReplaceSel(strRe);
						}
					} // while retry

					{
						int csLen = pDlg->m_CheckSum_tx.GetWindowTextLengthW();
						pDlg->m_CheckSum_tx.SetSel(csLen, csLen);
						pDlg->m_CheckSum_tx.ReplaceSel(_T("---------------------------\r\n"));
					}

					if (!pArg->Thread_run) { msgFailed = true; break; }

					if (ackOk) {
						frame.seq_num = (frame.seq_num + 1) % 2;
						frameNum++;
						offset += chunkLen;
					}
					else {
						// 최대 재전송 초과, 해당 메시지 포기
						CString strFail;
						strFail.Format(_T("FAILED seq=%d: max retries\r\n"), frame.seq_num);
						int csLen = pDlg->m_CheckSum_tx.GetWindowTextLengthW();
						pDlg->m_CheckSum_tx.SetSel(csLen, csLen);
						pDlg->m_CheckSum_tx.ReplaceSel(strFail);
						msgFailed = true;
						break;
					}
				} while (offset < totalLen);

				if (!msgFailed) {
					DWORD elapsed = GetTickCount() - startTime;
					int totalBytes = totalLen * (int)sizeof(TCHAR);
					CString strRate;
					if (elapsed > 0)
						strRate.Format(_T("전송률: %d bytes / %dms = %.2f KB/s\r\n===========================\r\n"),
							totalBytes, elapsed, (totalBytes * 1000.0) / 1024.0 / elapsed);
					else
						strRate.Format(_T("전송률: %d bytes / <1ms\r\n===========================\r\n"), totalBytes);
					int csLen = pDlg->m_CheckSum_tx.GetWindowTextLengthW();
					pDlg->m_CheckSum_tx.SetSel(csLen, csLen);
					pDlg->m_CheckSum_tx.ReplaceSel(strRate);
				}

				plist->RemoveAt(current_pos);
			}
		}
		Sleep(10);
	}
	return 0;
}


BOOL CUDPClientThdDlg::OnInitDialog()
{
	CDialogEx::OnInitDialog();

	ASSERT((IDM_ABOUTBOX & 0xFFF0) == IDM_ABOUTBOX);
	ASSERT(IDM_ABOUTBOX < 0xF000);

	CMenu* pSysMenu = GetSystemMenu(FALSE);
	if (pSysMenu != nullptr)
	{
		BOOL bNameValid;
		CString strAboutMenu;
		bNameValid = strAboutMenu.LoadString(IDS_ABOUTBOX);
		ASSERT(bNameValid);
		if (!strAboutMenu.IsEmpty())
		{
			pSysMenu->AppendMenu(MF_SEPARATOR);
			pSysMenu->AppendMenu(MF_STRING, IDM_ABOUTBOX, strAboutMenu);
		}
	}

	SetIcon(m_hIcon, TRUE);
	SetIcon(m_hIcon, FALSE);

	CStringList* newlist = new CStringList;
	arg1.pList = newlist;
	arg1.Thread_run = 1;
	arg1.pDlg = this;

	CStringList* newlist2 = new CStringList;
	arg2.pList = newlist2;
	arg2.Thread_run = 1;
	arg2.pDlg = this;

	WSADATA wsa;
	int error_code;
	if ((error_code = WSAStartup(MAKEWORD(2, 2), &wsa)) != 0) {
		TCHAR buffer[256];
		FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, error_code,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buffer, 256, NULL);
		AfxMessageBox(buffer, MB_ICONERROR);
	}

	// UDP 소켓 생성 (SOCK_DGRAM), 포트 0 = OS가 임의 포트 할당
	m_pDataSocket = new CDataSocket(this);
	if (!m_pDataSocket->Create(0, SOCK_DGRAM)) {
		AfxMessageBox(_T("UDP Socket create failed"));
		delete m_pDataSocket;
		m_pDataSocket = NULL;
	}

	pThread1 = AfxBeginThread(TXThread, (LPVOID)&arg1);
	pThread2 = AfxBeginThread(RXThread, (LPVOID)&arg2);

	return TRUE;
}

void CUDPClientThdDlg::OnSysCommand(UINT nID, LPARAM lParam)
{
	if ((nID & 0xFFF0) == IDM_ABOUTBOX)
	{
		CAboutDlg dlgAbout;
		dlgAbout.DoModal();
	}
	else
	{
		CDialogEx::OnSysCommand(nID, lParam);
	}
}

void CUDPClientThdDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this);

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialogEx::OnPaint();
	}
}

HCURSOR CUDPClientThdDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

// 수신 처리: ACK/NAK 프레임이면 TX 스레드를 깨우고,
// DATA 프레임이면 체크섬 검증 후 ACK/NAK 전송 및 재조합
void CUDPClientThdDlg::ProcessReceive(CDataSocket* pSocket, int nErrorCode)
{
	Frame recvFrame;
	CString fromIP;
	UINT fromPort;
	int nbytes;

	nbytes = pSocket->ReceiveFrom(&recvFrame, sizeof(Frame), fromIP, fromPort);
	if (nbytes <= 0)
		return;

	// 순수 ACK/NAK 수신, TX 스레드(Stop-and-Wait)에 신호 전달
	if (recvFrame.frame_type == 1 || recvFrame.frame_type == 2) {
		m_ackSeqNum = recvFrame.ack_num;
		m_ackType   = recvFrame.frame_type;
		SetEvent(m_hAckEvent);

		CString strLog;
		strLog.Format(_T("[%s] ack_num=%d\r\n"),
			recvFrame.frame_type == 1 ? _T("ACK") : _T("NAK"),
			recvFrame.ack_num);
		int len = m_CheckSum_tx.GetWindowTextLengthW();
		m_CheckSum_tx.SetSel(len, len);
		m_CheckSum_tx.ReplaceSel(strLog);
		return;
	}

	// DATA 프레임 처리

	// 피기배킹된 ACK가 포함된 DATA 프레임이면 TX 스레드에 신호 전달
	if (recvFrame.has_piggybacked_ack) {
		m_ackSeqNum = recvFrame.ack_num;
		m_ackType   = 1; // ACK
		SetEvent(m_hAckEvent);

		CString strLog;
		strLog.Format(_T("[PIGGY-ACK] ack_num=%d\r\n"), recvFrame.ack_num);
		int len = m_CheckSum_tx.GetWindowTextLengthW();
		m_CheckSum_tx.SetSel(len, len);
		m_CheckSum_tx.ReplaceSel(strLog);
	}

	unsigned short total = onesCompSum(&recvFrame, sizeof(Frame));
	bool valid = ((~total & 0xFFFF) == 0);

	CString strCS;
	strCS.Format(_T("RX seq=%d frame=%d%s: cs=0x%04X total=0x%04X [%s]\r\n"),
		recvFrame.seq_num, recvFrame.Frame_num,
		recvFrame.is_last_Frame ? _T("[L]") : _T(""),
		(unsigned short)recvFrame.checksum, total,
		valid ? _T("OK") : _T("ERR"));
	int csLen = m_CheckSum_rx.GetWindowTextLengthW();
	m_CheckSum_rx.SetSel(csLen, csLen);
	m_CheckSum_rx.ReplaceSel(strCS);

	if (!valid) {
		// NAK는 즉시 전송 (재전송을 빠르게 유도)
		Frame nakFrame;
		nakFrame.frame_type = 2; // NAK
		nakFrame.ack_num    = recvFrame.seq_num;
		nakFrame.checksum   = getChecksum(nakFrame);
		pSocket->SendTo(&nakFrame, sizeof(Frame), fromPort, fromIP);

		CString strAck;
		strAck.Format(_T("NAK ack_num=%d\r\n"), recvFrame.seq_num);
		csLen = m_CheckSum_rx.GetWindowTextLengthW();
		m_CheckSum_rx.SetSel(csLen, csLen);
		m_CheckSum_rx.ReplaceSel(strAck);
	}
	else {
		// 유효 DATA: ACK를 즉시 보내지 않고 피기배킹을 위해 보류
		m_pendingAckSeq  = recvFrame.seq_num;
		m_pendingAckTime = GetTickCount();

		CString strAck;
		strAck.Format(_T("ACK 보류(피기배킹 대기) ack_num=%d\r\n"), recvFrame.seq_num);
		csLen = m_CheckSum_rx.GetWindowTextLengthW();
		m_CheckSum_rx.SetSel(csLen, csLen);
		m_CheckSum_rx.ReplaceSel(strAck);
	}

	if (!valid)
		return; // 손상 프레임: 재조합/기대 seq 갱신 안 함

	// 중복 검출: ACK 분실로 송신측이 재전송한 경우 — 피기배킹 ACK가 이미 보류됐으므로 폐기만 함
	if (recvFrame.seq_num != m_expectedSeq)
	{
		CString strDup;
		strDup.Format(_T("중복 seq=%d (예상=%d) -> ACK 재보류 & 드랍\r\n"),
			recvFrame.seq_num, m_expectedSeq);
		csLen = m_CheckSum_rx.GetWindowTextLengthW();
		m_CheckSum_rx.SetSel(csLen, csLen);
		m_CheckSum_rx.ReplaceSel(strDup);
		return;
	}

	// 정상 순서 프레임: 다음 기대 seq 전진
	m_expectedSeq = (m_expectedSeq + 1) % 2;


	// 재조합 버퍼에 누적
	m_reassemBuf.push_back(recvFrame);

	if (recvFrame.is_last_Frame)
	{
		std::sort(m_reassemBuf.begin(), m_reassemBuf.end(),
			[](const Frame& a, const Frame& b) { return a.Frame_num < b.Frame_num; });

		CString fullMsg;
		for (const Frame& f : m_reassemBuf)
			fullMsg += f.p_buffer;
		fullMsg += _T("\r\n");

		m_reassemBuf.clear();

		rx_cs.Lock();
		arg2.pList->AddTail(fullMsg);
		rx_cs.Unlock();
	}
}

// IP와 메시지를 읽어 TX 큐에 추가하고 전송 이력창에 표시
void CUDPClientThdDlg::OnBnClickedButton1()
{
	if (m_pDataSocket == NULL) {
		AfxMessageBox(_T("Socket not ready"));
		return;
	}

	DWORD dwAddr;
	m_ipaddr.GetAddress(dwAddr);
	m_serverIP.Format(_T("%d.%d.%d.%d"),
		(dwAddr >> 24) & 0xFF,
		(dwAddr >> 16) & 0xFF,
		(dwAddr >> 8)  & 0xFF,
		 dwAddr        & 0xFF);

	CString tx_message;
	m_tx_edit_short.GetWindowTextW(tx_message);
	tx_message += _T("\r\n");

	tx_cs.Lock();
	arg1.pList->AddTail(tx_message);
	tx_cs.Unlock();

	m_tx_edit_short.SetWindowTextW(_T(""));
	m_tx_edit_short.SetFocus();

	int len = m_tx_edit.GetWindowTextLengthW();
	m_tx_edit.SetSel(len, len);
	m_tx_edit.ReplaceSel(tx_message);
}

// 스레드를 정지시키고 소켓을 종료한 뒤 다이얼로그 닫기
void CUDPClientThdDlg::OnBnClickedButton2()
{
	arg1.Thread_run = 0;
	arg2.Thread_run = 0;
	// ACK 대기 중인 TX 스레드가 있으면 즉시 깨워서 종료하도록 신호
	if (m_hAckEvent) {
		SetEvent(m_hAckEvent);
		Sleep(100); // 스레드가 루프를 빠져나갈 시간
		CloseHandle(m_hAckEvent);
		m_hAckEvent = NULL;
	}
	if (m_pDataSocket != NULL) {
		m_pDataSocket->Close();
		delete m_pDataSocket;
		m_pDataSocket = NULL;
	}
	CDialogEx::OnOK();
}
