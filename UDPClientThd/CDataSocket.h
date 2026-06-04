#pragma once


class CUDPClientThdDlg;


// UDP 수신 이벤트를 다이얼로그로 전달하는 소켓 클래스
class CDataSocket : public CSocket
{
public:

	CDataSocket(CUDPClientThdDlg* pDlg);
	virtual ~CDataSocket();

private:

	CUDPClientThdDlg* m_pDlg;

public:

	virtual void OnReceive(int nErrorCode); // 데이터 수신 시 다이얼로그의 ProcessReceive 호출
};
