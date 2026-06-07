
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

static void AppendToEdit(CEdit& edit, const CString& str)
{
	int len = edit.GetWindowTextLengthW();
	edit.SetSel(len, len);
	edit.ReplaceSel(str);
}

// CUDPServerThdDlg 대화 상자


// RX 스레드:
// 1. RX 큐에서 완성 메시지를 꺼내 수신 에디트박스에 출력
// 2. 피기배킹 타임아웃(1000ms) 감시 — 초과 시 순수 ACK 프레임 즉시 전송
UINT RXThread(LPVOID arg)
{
	const int PIGGY_TIMEOUT_MS = 1000;

	ThreadArg* pArg = (ThreadArg*)arg;
	CStringList* plist = pArg->pList;
	CUDPServerThdDlg* pDlg = (CUDPServerThdDlg*)pArg->pDlg;
	while (pArg->Thread_run) {
		POSITION pos = plist->GetHeadPosition();
		POSITION current_pos;
		while (pos != NULL) {
			current_pos = pos;
			// [1] RX 큐에서 메시지 꺼냄 (rx_cs 보호)
			rx_cs.Lock();
			CString str = plist->GetNext(pos);
			rx_cs.Unlock();

			AppendToEdit(pDlg->m_rx_edit, str);

			plist->RemoveAt(current_pos);
		}

		// [2] 보류 ACK 타임아웃 감시 — 1000ms 초과 시 순수 ACK 전송
		int pendingSeq = pDlg->m_pendingAckSeq;
		if (pendingSeq != -1 &&
			GetTickCount() - pDlg->m_pendingAckTime > PIGGY_TIMEOUT_MS)
		{
			client_cs.Lock();
			CString strAddr = pDlg->m_strClientAddr;
			UINT nPort = pDlg->m_nClientPort;
			client_cs.Unlock();

			if (!strAddr.IsEmpty() && pDlg->m_pUDPSocket != NULL) {
				// [2] 타임아웃 초과 — 순수 ACK 프레임 구성 후 전송
				Frame ackFrame;
				ackFrame.frame_type = 1; // ACK
				ackFrame.ack_num    = pendingSeq;
				ackFrame.checksum   = getChecksum(ackFrame);
				pDlg->m_pendingAckSeq = -1;
				pDlg->m_pUDPSocket->SendTo(&ackFrame, sizeof(Frame), nPort, strAddr);

				CString strLog;
				strLog.Format(_T("STANDALONE-ACK ack_num=%d\r\n"), pendingSeq);
				AppendToEdit(pDlg->m_CheckSum_rx, strLog);
				AppendToEdit(pDlg->m_CheckSum_rx, _T("---------------------------\r\n"));
			}
		}

		Sleep(10);
	}
	return 0;
}

// TX 스레드:
// 1. TX 큐에서 메시지를 꺼냄
// 2. 메시지를 청크 단위로 분할해 순서대로 전송
// 3. DATA 프레임 구성 및 피기배킹 ACK 탑재
// 4. Stop-and-Wait: 전송 후 ACK 대기, 타임아웃/NAK 시 최대 5회 재전송
// 5. 전송 완료 후 전송률(KB/s) 계산 출력
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
			// [1] TX 큐에서 메시지 꺼냄 (tx_cs 보호)
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
				DWORD startTime = GetTickCount();

				// [2] 청크 단위로 메시지 전체를 순회, is_last_Frame으로 마지막 청크 표시
				do {
					if (!pArg->Thread_run) { msgFailed = true; break; }

					CString chunk = str.Mid(offset, chunkLen);
					int retries = 0;
					bool ackOk  = false;

					while (!ackOk && retries <= MAX_RETRY && pArg->Thread_run) {
						// [3] DATA 프레임 구성: seq_num, frame_type, Frame_num, is_last_Frame 설정
						_tcscpy_s(frame.p_buffer, _countof(frame.p_buffer), chunk);
						frame.frame_type    = 0; // DATA
						frame.Frame_num     = frameNum;
						frame.is_last_Frame = (offset + chunkLen >= totalLen) ? 1 : 0;

						// [3] 보류 중인 ACK가 있으면 DATA 프레임에 피기배킹
						frame.has_piggybacked_ack = 0;
						frame.ack_num = 0;
						int pendingAck = pDlg->m_pendingAckSeq;
						if (pendingAck != -1) {
							frame.has_piggybacked_ack = 1;
							frame.ack_num = pendingAck;
							pDlg->m_pendingAckSeq = -1;

							AppendToEdit(pDlg->m_CheckSum_rx, _T("---------------------------\r\n"));
						}

						frame.checksum = getChecksum(frame);

						// [4] SendTo 전에 리셋해야 이전 잔여 이벤트를 오인하지 않음
						ResetEvent(pDlg->m_hAckEvent);
						pDlg->m_pUDPSocket->SendTo(&frame, sizeof(Frame), nPort, strAddr);

						// [4] 최초 전송이면 TX 로그, 재전송이면 RETRY#N 로그
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
						AppendToEdit(pDlg->m_CheckSum_tx, strLog);

						// [4] ACK/NAK 대기 — ProcessReceive가 SetEvent로 깨움, 2000ms 초과 시 TIMEOUT
						DWORD wr = WaitForSingleObject(pDlg->m_hAckEvent, TIMEOUT_MS);

						if (wr == WAIT_OBJECT_0
							&& pDlg->m_ackSeqNum == frame.seq_num
							&& pDlg->m_ackType   == 1)
						{
							ackOk = true;
							CString strAck;
							strAck.Format(_T("[ACK] seq=%d OK\r\n"), frame.seq_num);
							AppendToEdit(pDlg->m_CheckSum_tx, strAck);
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
							AppendToEdit(pDlg->m_CheckSum_tx, strRe);
						}
					} // while retry

					AppendToEdit(pDlg->m_CheckSum_tx, _T("---------------------------\r\n"));

					if (!pArg->Thread_run) { msgFailed = true; break; }

					if (ackOk) {
						frame.seq_num = (frame.seq_num + 1) % 2; // [4] 1-bit 교대 seq: 0→1→0 순환
						frameNum++;
						offset += chunkLen;
					}
					else {
						// [4] 최대 재전송 횟수(5회) 초과, 해당 메시지 포기
						CString strFail;
						strFail.Format(_T("FAILED seq=%d: max retries\r\n"), frame.seq_num);
						AppendToEdit(pDlg->m_CheckSum_tx, strFail);
						msgFailed = true;
						break;
					}
				} while (offset < totalLen);

				// [5] 전송 완료 후 전송률(KB/s) 계산 출력
				if (!msgFailed) {
					DWORD elapsed = GetTickCount() - startTime;
					int totalBytes = totalLen * (int)sizeof(TCHAR);
					CString strRate;
					if (elapsed > 0)
						strRate.Format(_T("전송률: %d bytes / %dms = %.2f KB/s\r\n===========================\r\n"),
							totalBytes, elapsed, (totalBytes * 1000.0) / 1024.0 / elapsed);
					else
						strRate.Format(_T("전송률: %d bytes / <1ms\r\n===========================\r\n"), totalBytes);
					AppendToEdit(pDlg->m_CheckSum_tx, strRate);
				}

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
	m_expectedSeq = 0;
	m_pendingAckSeq = -1;
	m_pendingAckTime = 0;
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

// ProcessReceive:
// 1. 소켓에서 프레임 수신
// 2. 순수 ACK/NAK 수신 시 TX 스레드에 SetEvent로 신호 전달
// 3. DATA 프레임의 피기배킹 ACK 처리 — TX 스레드에 SetEvent로 신호 전달
// 4. 체크섬 검증 — 손상 시 NAK 즉시 전송, 유효 시 ACK 보류(피기배킹 대기)
// 5. 중복 프레임 검출 — seq 불일치 시 드랍
// 6. 정상 프레임 재조합 — 마지막 프레임이면 완성 메시지를 RX 큐에 추가
void CUDPServerThdDlg::ProcessReceive(CDataSocket* pSocket, int nErrorCode)
{
	Frame recvFrame;
	CString fromIP;
	UINT fromPort;
	int nbytes;

	// [1] 소켓에서 프레임 수신
	nbytes = pSocket->ReceiveFrom(&recvFrame, sizeof(Frame), fromIP, fromPort);
	if (nbytes <= 0)
		return;

	// [2] 순수 ACK/NAK 수신 — TX 스레드에 SetEvent로 신호 전달
	if (recvFrame.frame_type == 1 || recvFrame.frame_type == 2) {
		m_ackSeqNum = recvFrame.ack_num;
		m_ackType   = recvFrame.frame_type;
		SetEvent(m_hAckEvent);

		CString strLog;
		strLog.Format(_T("[%s] ack_num=%d\r\n"),
			recvFrame.frame_type == 1 ? _T("ACK") : _T("NAK"),
			recvFrame.ack_num);
		AppendToEdit(m_CheckSum_tx, strLog);
		return;
	}

	// 클라이언트 주소 갱신 (TX 스레드가 SendTo에 사용)
	client_cs.Lock();
	m_strClientAddr = fromIP;
	m_nClientPort   = fromPort;
	client_cs.Unlock();

	// [3] 피기배킹 ACK 처리 — TX 스레드에 SetEvent로 신호 전달
	if (recvFrame.has_piggybacked_ack) {
		m_ackSeqNum = recvFrame.ack_num;
		m_ackType   = 1; // ACK
		SetEvent(m_hAckEvent);

		CString strLog;
		strLog.Format(_T("[PIGGY-ACK] ack_num=%d\r\n"), recvFrame.ack_num);
		AppendToEdit(m_CheckSum_tx, strLog);
	}

	// [4] 체크섬 검증 — ones complement 합산 후 ~결과가 0x0000이면 유효
	unsigned short total = onesCompSum(&recvFrame, sizeof(Frame));
	bool valid = ((~total & 0xFFFF) == 0);

	CString strCS;
	strCS.Format(_T("RX seq=%d frame=%d%s: cs=0x%04X total=0x%04X [%s]\r\n"),
		recvFrame.seq_num, recvFrame.Frame_num,
		recvFrame.is_last_Frame ? _T("[L]") : _T(""),
		(unsigned short)recvFrame.checksum, total,
		valid ? _T("OK") : _T("ERR"));
	AppendToEdit(m_CheckSum_rx, strCS);

	if (!valid) {
		// [4] 체크섬 오류 — NAK 즉시 전송으로 재전송 유도
		Frame nakFrame;
		nakFrame.frame_type = 2; // NAK
		nakFrame.ack_num    = recvFrame.seq_num;
		nakFrame.checksum   = getChecksum(nakFrame);
		pSocket->SendTo(&nakFrame, sizeof(Frame), fromPort, fromIP);

		CString strAck;
		strAck.Format(_T("NAK ack_num=%d\r\n"), recvFrame.seq_num);
		AppendToEdit(m_CheckSum_rx, strAck);
	}
	else {
		// [4] 체크섬 정상 — ACK 즉시 전송 대신 피기배킹 대기
		m_pendingAckSeq  = recvFrame.seq_num;
		m_pendingAckTime = GetTickCount();

		CString strAck;
		strAck.Format(_T("ACK 보류(피기배킹 대기) ack_num=%d\r\n"), recvFrame.seq_num);
		AppendToEdit(m_CheckSum_rx, strAck);
	}

	if (!valid)
		return; // [4] 손상 프레임은 재조합/기대 seq 갱신 없이 버림

	// [5] 중복 프레임 검출 — seq 불일치(ACK 분실로 재전송된 경우) 시 드랍, ACK 보류는 이미 됨
	if (recvFrame.seq_num != m_expectedSeq)
	{
		CString strDup;
		strDup.Format(_T("중복 seq=%d (예상=%d) -> ACK 재보류 & 드랍\r\n"),
			recvFrame.seq_num, m_expectedSeq);
		AppendToEdit(m_CheckSum_rx, strDup);
		return;
	}

	// [6] 정상 순서 프레임 — 다음 기대 seq 전진
	m_expectedSeq = (m_expectedSeq + 1) % 2;

	// [6] 재조합 버퍼에 청크 누적, 마지막 프레임이면 완성 메시지를 RX 큐에 추가
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
