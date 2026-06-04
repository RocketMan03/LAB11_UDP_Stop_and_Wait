// CDataSocket.cpp

#include "pch.h"
#include "UDPClientThd.h"
#include "CDataSocket.h"
#include "UDPClientThdDlg.h"

CDataSocket::CDataSocket(CUDPClientThdDlg* pDlg)
{
	m_pDlg = pDlg;
}

CDataSocket::~CDataSocket()
{
}

// 데이터 수신 이벤트 — 다이얼로그에 처리 위임
void CDataSocket::OnReceive(int nErrorCode)
{
	CSocket::OnReceive(nErrorCode);
	m_pDlg->ProcessReceive(this, nErrorCode);
}
