
// UDPServerThdDlg.cpp: 구현 파일
//

#include "pch.h"
#include "framework.h"
#include "UDPServerThd.h"
#include "UDPServerThdDlg.h"
#include "afxdialogex.h"
#include "DataSocket.h"
#ifdef _DEBUG
#define new DEBUG_NEW
#endif

CCriticalSection rx_cs, tx_cs, client_cs; // RX/TX 큐 및 클라이언트 주소 접근 보호용

// 응용 프로그램 정보에 사용되는 CAboutDlg 대화 상자입니다.

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


// 16비트 단위 ones complement 합산 (캐리 순환올림 포함)
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

// checksum 필드를 0으로 두고 프레임 전체를 합산 후 1의 보수를 취함
int getChecksum(Frame frame)
{
	frame.checksum = 0;
	return (int)(unsigned short)(~onesCompSum(&frame, sizeof(Frame)));
}

// CUDPServerThdDlg 대화 상자


// RX 스레드: RX 큐에서 문자열을 꺼내 수신 에디트박스에 출력
UINT RXThread(LPVOID arg)
{
	ThreadArg* pArg = (ThreadArg*)arg;
	CStringList* plist = pArg->pList;
	CUDPServerThdDlg* pDlg = (CUDPServerThdDlg*)pArg->pDlg;
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
	CUDPServerThdDlg* pDlg = (CUDPServerThdDlg*)pArg->pDlg;

	while (pArg->Thread_run) {
		POSITION pos = plist->GetHeadPosition();
		POSITION current_pos;
		while (pos != NULL) {
			current_pos = pos;
			tx_cs.Lock();
			CString str = plist->GetNext(pos);
			tx_cs.Unlock();

			client_cs.Lock();
			CString strAddr = pDlg->m_strClientAddr;
			UINT nPort = pDlg->m_nClientPort;
			client_cs.Unlock();

			if (pDlg->m_pUDPSocket != NULL && !strAddr.IsEmpty())
			{
				const int chunkLen = _countof(frame.p_buffer) - 1;
				int offset = 0;
				int totalLen = str.GetLength();
				int frameNum = 0;
				bool msgFailed = false;

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
						frame.checksum      = getChecksum(frame);

						// 이전 ACK 신호 초기화 후 전송
						ResetEvent(pDlg->m_hAckEvent);
						pDlg->m_pUDPSocket->SendTo(&frame, sizeof(Frame), nPort, strAddr);

						// TX 로그
						CString strLog;
						if (retries == 0)
							strLog.Format(_T("TX  seq=%d frame=%d%s cs=0x%04X\r\n"),
								frame.seq_num, frame.Frame_num,
								frame.is_last_Frame ? _T("[L]") : _T(""),
								(unsigned short)frame.checksum);
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

					if (!pArg->Thread_run) { msgFailed = true; break; }

					if (ackOk) {
						frame.seq_num++;
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

				plist->RemoveAt(current_pos);
			}
		}
		Sleep(10);
	}
	return 0;
}


CUDPServerThdDlg::CUDPServerThdDlg(CWnd* pParent /*=nullptr*/)
	: CDialogEx(IDD_UDPSERVERTHD_DIALOG, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	m_pUDPSocket = NULL;
	m_nClientPort = 0;
	pThread1 = NULL;
	pThread2 = NULL;
	m_hAckEvent = CreateEvent(NULL, FALSE, FALSE, NULL); // auto-reset
	m_ackSeqNum = -1;
	m_ackType = 0;
	m_expectedSeq = 0;   // 추가
}

void CUDPServerThdDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialogEx::DoDataExchange(pDX);
	DDX_Control(pDX, IDC_EDIT1, m_edit);
	DDX_Control(pDX, IDC_EDIT2, m_tx_edit);
	DDX_Control(pDX, IDC_EDIT3, m_rx_edit);
	DDX_Control(pDX, IDC_EDIT4, m_CheckSum_rx);
	DDX_Control(pDX, IDC_EDIT5, m_CheckSum_tx);
}

BEGIN_MESSAGE_MAP(CUDPServerThdDlg, CDialogEx)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	ON_BN_CLICKED(IDC_CLOSE, &CUDPServerThdDlg::OnBnClickedClose)
	ON_BN_CLICKED(IDC_SEND, &CUDPServerThdDlg::OnBnClickedSend)
END_MESSAGE_MAP()


// Winsock 초기화, UDP 소켓 생성(포트 8000 바인드), TX/RX 스레드 시작
BOOL CUDPServerThdDlg::OnInitDialog()
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

	// UDP 소켓 생성 후 포트 8000에 바인드 (SOCK_DGRAM)
	m_pUDPSocket = new CDataSocket(this);
	if (m_pUDPSocket->Create(8000, SOCK_DGRAM)) {
		AfxMessageBox(_T("UDP 서버를 시작합니다. (포트 8000)"), MB_ICONINFORMATION);
		pThread1 = AfxBeginThread(TXThread, (LPVOID)&arg1);
		pThread2 = AfxBeginThread(RXThread, (LPVOID)&arg2);
		return TRUE;
	}
	else {
		int err = m_pUDPSocket->GetLastError();
		TCHAR errBuf[256];
		FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, err,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), errBuf, 256, NULL);
		AfxMessageBox(errBuf, MB_ICONERROR);
		delete m_pUDPSocket;
		m_pUDPSocket = NULL;
	}

	AfxMessageBox(_T("소켓 생성 실패. 프로그램을 종료합니다."), MB_ICONERROR);
	return FALSE;
}

void CUDPServerThdDlg::OnSysCommand(UINT nID, LPARAM lParam)
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

void CUDPServerThdDlg::OnPaint()
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

HCURSOR CUDPServerThdDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}

// 수신 처리: ACK/NAK 프레임이면 TX 스레드를 깨우고,
// DATA 프레임이면 체크섬 검증 후 ACK/NAK 전송 및 재조합
void CUDPServerThdDlg::ProcessReceive(CDataSocket* pSocket, int nErrorCode)
{
	Frame recvFrame;
	CString fromIP;
	UINT fromPort;
	int nbytes;

	nbytes = pSocket->ReceiveFrom(&recvFrame, sizeof(Frame), fromIP, fromPort);
	if (nbytes <= 0)
		return;

	// ACK/NAK 수신, TX 스레드(Stop-and-Wait)에 신호 전달
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

	// 클라이언트 주소 갱신 (TX 스레드가 SendTo에 사용)
	client_cs.Lock();
	m_strClientAddr = fromIP;
	m_nClientPort   = fromPort;
	client_cs.Unlock();

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

	// ACK 또는 NAK 전송
	// ACK 또는 NAK 전송 (중복 프레임이라도 ACK는 다시 보내야 송신측이 진행함)
	Frame ackFrame;
	ackFrame.frame_type = valid ? 1 : 2;
	ackFrame.ack_num = recvFrame.seq_num;
	ackFrame.checksum = getChecksum(ackFrame);
	
	
	//pSocket->SendTo(&ackFrame, sizeof(Frame), fromPort, fromIP);

	// (검증용 코드) 4번째 유효 프레임의 ACK를 일부러 누락
	static int s_ackDropTest = 0;
	bool dropAck = (++s_ackDropTest % 4 == 0);
	if (!dropAck)
		pSocket->SendTo(&ackFrame, sizeof(Frame), fromPort, fromIP);

	CString strAck;
	strAck.Format(_T("%s ack_num=%d\r\n"),
		valid ? _T("ACK") : _T("NAK"), recvFrame.seq_num);
	csLen = m_CheckSum_rx.GetWindowTextLengthW();
	m_CheckSum_rx.SetSel(csLen, csLen);
	m_CheckSum_rx.ReplaceSel(strAck);

	if (!valid)
		return; // 손상 프레임: 재조합/기대 seq 갱신 안 함

	// --- 중복 검출 (Stop-and-Wait) ---
	// ACK가 분실되어 송신측이 같은 프레임을 재전송한 경우.
	// 위에서 ACK는 다시 보냈으므로, 여기서는 폐기만 한다 (재조합 X).
	if (recvFrame.seq_num != m_expectedSeq)
	{
		CString strDup;
		strDup.Format(_T("중복 seq=%d (예상=%d) -> re-Ark & 드랍\r\n"),
			recvFrame.seq_num, m_expectedSeq);
		csLen = m_CheckSum_rx.GetWindowTextLengthW();
		m_CheckSum_rx.SetSel(csLen, csLen);
		m_CheckSum_rx.ReplaceSel(strDup);
		return;
	}

	// 정상 순서 프레임: 다음 기대 seq 전진
	m_expectedSeq++;

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

// 전송 버튼: 입력 메시지를 TX 큐에 추가하고 전송 이력에 표시
void CUDPServerThdDlg::OnBnClickedSend()
{
	CString tx_message;
	m_edit.GetWindowTextW(tx_message);
	tx_message += _T("\r\n");

	tx_cs.Lock();
	arg1.pList->AddTail(tx_message);
	tx_cs.Unlock();

	m_edit.SetWindowTextW(_T(""));
	m_edit.SetFocus();

	int len = m_tx_edit.GetWindowTextLengthW();
	m_tx_edit.SetSel(len, len);
	m_tx_edit.ReplaceSel(tx_message);
}

// 종료 버튼: 스레드 중지 후 UDP 소켓 닫기
void CUDPServerThdDlg::OnBnClickedClose()
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
	if (m_pUDPSocket != NULL) {
		m_pUDPSocket->Close();
		delete m_pUDPSocket;
		m_pUDPSocket = NULL;
	}
}
