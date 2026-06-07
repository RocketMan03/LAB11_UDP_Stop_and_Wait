
// UDPServerThdDlg.h: 헤더 파일
//

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

struct ThreadArg
{
	CStringList* pList;
	CDialogEx* pDlg;
	int Thread_run;
};

class CDataSocket;

// CUDPServerThdDlg 대화 상자
class CUDPServerThdDlg : public CDialogEx
{
public:
	CUDPServerThdDlg(CWnd* pParent = nullptr);

#ifdef AFX_DESIGN_TIME
	enum { IDD = IDD_UDPSERVERTHD_DIALOG };
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
	afx_msg void OnBnClickedSend();
	afx_msg void OnBnClickedClose();

	CWinThread* pThread1, * pThread2;
	ThreadArg arg1, arg2;

	CDataSocket* m_pUDPSocket;
	CString m_strClientAddr;   // 마지막으로 수신한 클라이언트 IP
	UINT m_nClientPort;        // 마지막으로 수신한 클라이언트 포트

	void ProcessReceive(CDataSocket* pSocket, int nErrorCode);

	CEdit m_edit;      // IDC_EDIT1: 단문 TX 입력
	CEdit m_tx_edit;   // IDC_EDIT2: TX 이력
	CEdit m_rx_edit;   // IDC_EDIT3: RX 표시
	CEdit m_CheckSum_rx;
	CEdit m_CheckSum_tx;

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
