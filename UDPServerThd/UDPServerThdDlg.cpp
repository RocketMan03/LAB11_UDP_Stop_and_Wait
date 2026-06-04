
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

// TX 스레드: TX 큐에서 문자열을 꺼내 마지막 클라이언트 주소로 SendTo
UINT TXThread(LPVOID arg)
{
	Frame frame;
	frame.ack_num = 0;
	frame.checksum = 0;
	frame.seq_num = 0;

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

			
			
			//입력받은 긴 문자열(str)을 안전한 크기인 청크(Chunk) 단위로 쪼개서 
			// 개별 프레임의 페이로드(p_buffer)에 나누어 담아 연속으로 전송
			// checkSum 진행후 seq_num++ 까지 진행

			// \r | \n | \0 가 붙으므로, 6바이트손해(한칸당2 바이트), 프레임의 페이로드는 사실상 16 - 6 = 10바이트임

			if (pDlg->m_pUDPSocket != NULL && !strAddr.IsEmpty())
			{
				const int chunkLen = _countof(frame.p_buffer) - 1;
				int offset = 0;
				int totalLen = str.GetLength();
				int frameNum = 0;
				do {
					CString chunk = str.Mid(offset, chunkLen);
					_tcscpy_s(frame.p_buffer, _countof(frame.p_buffer), chunk);
					frame.Frame_num = frameNum;
					frame.is_last_Frame = (offset + chunkLen >= totalLen) ? 1 : 0;
					frame.checksum = getChecksum(frame);
					pDlg->m_pUDPSocket->SendTo(&frame, sizeof(Frame), nPort, strAddr);

					CString strCS;
					strCS.Format(_T("TX seq=%d frame=%d%s: checksum=0x%04X\r\n"),
						frame.seq_num, frame.Frame_num,
						frame.is_last_Frame ? _T("[LAST]") : _T(""),
						(unsigned short)frame.checksum);
					int csLen = pDlg->m_CheckSum_tx.GetWindowTextLengthW();
					pDlg->m_CheckSum_tx.SetSel(csLen, csLen);
					pDlg->m_CheckSum_tx.ReplaceSel(strCS);

					frame.seq_num++;
					frameNum++;
					offset += chunkLen;
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

// Frame 단위로 수신 → 체크섬 검증 → 재조합 버퍼 저장 → is_last_Frame 시 완성 메시지를 RX 큐에 추가
void CUDPServerThdDlg::ProcessReceive(CDataSocket* pSocket, int nErrorCode)
{
	Frame recvFrame;
	CString fromIP;
	UINT fromPort;
	int nbytes;

	nbytes = pSocket->ReceiveFrom(&recvFrame, sizeof(Frame), fromIP, fromPort);
	if (nbytes <= 0)
		return;

	// 마지막 클라이언트 주소 갱신 (TX 스레드가 SendTo에 사용)
	client_cs.Lock();
	m_strClientAddr = fromIP;
	m_nClientPort = fromPort;
	client_cs.Unlock();

	unsigned short total = onesCompSum(&recvFrame, sizeof(Frame));
	bool valid = ((~total & 0xFFFF) == 0);

	CString strCS;
	strCS.Format(_T("RX seq=%d frame=%d%s: checksum=0x%04X total=0x%04X [%s]\r\n"),
		recvFrame.seq_num, recvFrame.Frame_num,
		recvFrame.is_last_Frame ? _T("[LAST]") : _T(""),
		(unsigned short)recvFrame.checksum, total,
		valid ? _T("OK") : _T("ERR"));
	int csLen = m_CheckSum_rx.GetWindowTextLengthW();
	m_CheckSum_rx.SetSel(csLen, csLen);
	m_CheckSum_rx.ReplaceSel(strCS);

	if (!valid)
		return; // 체크섬 오류 프레임 폐기

	// 재조합 버퍼에 누적
	m_reassemBuf.push_back(recvFrame);

	if (recvFrame.is_last_Frame)
	{
		// Frame_num 순으로 정렬 (UDP는 순서 보장 없음)
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
	if (m_pUDPSocket != NULL) {
		m_pUDPSocket->Close();
		delete m_pUDPSocket;
		m_pUDPSocket = NULL;
	}
}
