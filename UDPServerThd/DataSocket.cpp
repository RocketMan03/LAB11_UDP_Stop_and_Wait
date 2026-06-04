// DataSocket.cpp: 구현 파일
//

#include "pch.h"
#include "UDPServerThd.h"
#include "UDPServerThdDlg.h"
#include "DataSocket.h"

CDataSocket::CDataSocket(CUDPServerThdDlg* pDlg)
{
	m_pDlg = pDlg;
}

CDataSocket::~CDataSocket()
{
}

// 데이터 수신 시 다이얼로그의 ProcessReceive에 위임
void CDataSocket::OnReceive(int nErrorCode)
{
	CSocket::OnReceive(nErrorCode);
	m_pDlg->ProcessReceive(this, nErrorCode);
}
