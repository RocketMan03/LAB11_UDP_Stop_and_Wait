#pragma once

class CUDPServerThdDlg;

class CDataSocket : public CSocket
{
public:
	CDataSocket(CUDPServerThdDlg* pDlg);
	virtual ~CDataSocket();
	CUDPServerThdDlg* m_pDlg;
	virtual void OnReceive(int nErrorCode);
};
