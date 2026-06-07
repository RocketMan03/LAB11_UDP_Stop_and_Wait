
// UDPClientThdDlg.h

#pragma once
#include "afxcoll.h"
#include "afxwin.h"
#include <vector>
#include <algorithm>


// frame_type: 0=DATA, 1=ACK, 2=NAK
struct Frame
{
	int seq_num;
	int ack_num;
	int checksum;
	int Frame_num;            // 메시지 내 프레임 번호 (0부터)
	int is_last_Frame;        // 마지막 프레임이면 1, 아니면 0
	int frame_type;           // 0=DATA, 1=ACK, 2=NAK
	int has_piggybacked_ack;  // 1이면 ack_num에 피기배킹된 ACK 포함
	TCHAR p_buffer[8];        // 8 TCHAR = 16바이트 페이로드

	Frame() {
		seq_num = 0;
		ack_num = 0;
		checksum = 0;
		Frame_num = 0;
		is_last_Frame = 0;
		frame_type = 0;
		has_piggybacked_ack = 0;
		memset(p_buffer, 0, sizeof(p_buffer));
	}
};

// TX/RX 스레드에 전달하는 인자 구조체
struct ThreadArg
{
	CStringList* pList;  // 송수신 메시지 큐
	CDialogEx*   pDlg;   // 메인 다이얼로그 포인터
	int Thread_run;      // 스레드 실행 플래그 (0이면 종료)
};


class CDataSocket;


// UDP 클라이언트 메인 다이얼로그 — 송수신/스레드 관리
class CUDPClientThdDlg : public CDialogEx
{
public:
	CUDPClientThdDlg(CWnd* pParent = nullptr);

#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_UDPCLIENTTHD_DIALOG };
#endif

	protected:
	virtual void DoDataExchange(CDataExchange* pDX);

protected:
	HICON m_hIcon;

	virtual BOOL OnInitDialog();
	afx_msg void OnSysCommand(UINT nID, LPARAM lParam);
	afx_msg void OnPaint();
	afx_msg HCURSOR OnQueryDragIcon();
	DECLARE_MESSAGE_MAP()

public:
	CWinThread* pThread1; // TX 스레드
	CWinThread* pThread2; // RX 스레드

	ThreadArg arg1; // TX 스레드 인자
	ThreadArg arg2; // RX 스레드 인자

	CDataSocket* m_pDataSocket; // UDP 소켓
	CString m_serverIP;         // 전송 대상 서버 IP

	void ProcessReceive(CDataSocket* pSocket, int nErrorCode); // 수신 데이터를 RX 큐에 추가

	CIPAddressCtrl m_ipaddr;   // IP 주소 입력 컨트롤
	CEdit m_tx_edit_short;     // 전송할 메시지 입력창
	CEdit m_tx_edit;           // 전송 이력 표시창
	CEdit m_rx_edit;           // 수신 메시지 표시창

	afx_msg void OnBnClickedButton1(); // Send 버튼 — 메시지를 TX 큐에 추가
	afx_msg void OnBnClickedButton2(); // Close 버튼 — 스레드 정지 및 소켓 종료

	CEdit m_CheckSum_tx;
	CEdit m_CheckSum_rx;

	std::vector<Frame> m_reassemBuf; // 수신 프레임 재조합 버퍼

	// Stop-and-Wait ACK 동기화
	HANDLE m_hAckEvent;       // TX 스레드가 ACK/NAK 도착을 기다리는 이벤트
	volatile int m_ackSeqNum; // 수신된 ACK/NAK의 ack_num
	volatile int m_ackType;   // 1=ACK, 2=NAK
	int m_expectedSeq;        // 다음에 받을 차례인 seq (중복 검출용)

	// 피기배킹: 보류 중인 ACK
	volatile int m_pendingAckSeq; // 보류 중인 ACK seq (-1 = 없음)
	DWORD m_pendingAckTime;       // 보류 ACK 생성 시각 (GetTickCount)
};
