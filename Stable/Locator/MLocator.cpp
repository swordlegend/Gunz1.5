#include "StdAfx.h"
#include ".\mlocator.h"
#include "MLocatorDBMgr.h"
#include "CustomDefine.h"
#include "MSafeUDP.h"
#include "mmsystem.h"
#include "Msg.h"
#include "MSharedCommandTable.h"
#include "MBlobArray.h"
#include "MLocatorConfig.h"
#include "MLocatorUDP.h"
#include "MCommandCommunicator.h"
#include "MErrorTable.h"
#include "MCommandBuilder.h"
#include "MUtil.h"
#include "MLocatorStatistics.h"
#include "MServerStatus.h"
#include "MLogManager.h"

#pragma comment( lib, "winmm.lib" )

MLocator* g_pMainLocator = NULL;
void SetMainLocator( MLocator* pLocator ) { g_pMainLocator = pLocator; }
MLocator* GetMainLocator() { return g_pMainLocator; }


MLocator::MLocator(void)
{
	m_pDBMgr							= 0; 
	m_pSafeUDP							= 0;
	m_dwLastServerStatusUpdatedTime		= timeGetTime();
	m_dwLastUDPManagerUpdateTime		= timeGetTime();
	m_vpServerStatusInfoBlob			= 0;
	m_nLastGetServerStatusCount			= 0;
    m_nServerStatusInfoBlobSize			= 0;
	m_pServerStatusMgr					= 0;
	m_pRecvUDPManager					= 0;
	m_pSendUDPManager					= 0;
	m_pBlockUDPManager					= 0;
	m_nRecvCount						= 0;
	m_nSendCount						= 0;
	m_nDuplicatedCount					= 0;
	m_dwLastLocatorStatusUpdatedTime	= timeGetTime();

	m_This = GetLocatorConfig()->GetLocatorUID();

	InitializeCriticalSection( &m_csCommandQueueLock );
}

MLocator::~MLocator(void)
{
	Destroy();

	DeleteCriticalSection( &m_csCommandQueueLock );
}


bool MLocator::Create()
{
	SetMainLocator( this );
	OnRegisterCommand( &m_CommandManager );

	if( !GetLocatorConfig()->IsInitCompleted() )
	{
		if( !GetLocatorConfig()->LoadConfig() )
			return false;
	}
		
	if( !InitDBMgr() )
	{
		mlog( "MLocator::Create - DB초기화 실패.\n" );
		return false;
	}

	if( !InitServerStatusMgr() )
	{
		mlog( "MLocator::Create - ServerStatusMgr 멤버 초기화 실패.\n" );
		return false;
	}

	if( !InitUDPManager() )
	{
		mlog( "MLocator::Create - UDP Manager 멤버 초기화 실패.\n" );
		return false;
	}

	if( !InitSafeUDP() )
	{
		mlog( "MLocator::Create - SafeUDP초기화 실패.\n" );
		return false;
	}

#ifdef _DEBUG
	InitDebug();
#endif

	mlog( "MLocator create.\n" );

	return true;
}


bool MLocator::InitDBMgr()
{
	if( 0 != m_pDBMgr )
		ReleaseDBMgr();

	m_pDBMgr = new MLocatorDBMgr;
	if( 0 != m_pDBMgr )
	{
		const CString strDSNString = m_pDBMgr->BuildDSNString( GetLocatorConfig()->GetDBDSN(),
															   GetLocatorConfig()->GetDBUserName(), 
															   GetLocatorConfig()->GetDBPassword() );
		const bool bConnect = m_pDBMgr->Connect( strDSNString );
		if( bConnect )
		{
			GetDBServerStatus( timeGetTime(), true );

			return m_pDBMgr->StartUpLocaterStauts( GetLocatorConfig()->GetLocatorID(), 
				GetLocatorConfig()->GetLocatorIP(), 
				GetLocatorConfig()->GetLocatorPort(),
				GetLocatorConfig()->GetMaxElapsedUpdateServerStatusTime() );
		}
	}

	return false;
}


bool MLocator::InitSafeUDP()
{
	if( 0 == m_pSafeUDP )
	{
		m_pSafeUDP = new MSafeUDP;
		if( 0 != m_pSafeUDP )
		{
			if( m_pSafeUDP->Create(true, GetLocatorConfig()->GetLocatorPort()) )
			{
				m_pSafeUDP->SetCustomRecvCallback( UDPSocketRecvEvent );
				return true;
			}
		}
	}

	return false;
}


bool MLocator::InitServerStatusMgr()
{
	if( 0 == m_pServerStatusMgr )
	{
		m_pServerStatusMgr = new MServerStatusMgr;
		if( 0 == m_pServerStatusMgr ) 
			return false;

		if( 0 != GetLocatorDBMgr() )
			GetDBServerStatus( 0, true );
		else
			ASSERT( 0 && "시작시에 DB의 정보를 가져오는것이 좋음." );
		
		return true;
	}

	return false;
}

bool MLocator::InitUDPManager()
{
	bool bRet = true;
	m_pRecvUDPManager = new MUDPManager;
	if( (0 == m_pRecvUDPManager) && (bRet) ) bRet = false;
	m_pSendUDPManager = new MUDPManager;
	if( (0 == m_pSendUDPManager) && (bRet) ) bRet = false;
	m_pBlockUDPManager = new MUDPManager;
	if( (0 == m_pBlockUDPManager) && (bRet) ) bRet = false;

	if( !bRet )
	{
		delete m_pRecvUDPManager;
		delete m_pSendUDPManager;
		delete m_pBlockUDPManager;

		m_pRecvUDPManager = 0;
		m_pSendUDPManager = 0;
		m_pBlockUDPManager = 0;

		return false;
	}

	InitMemPool( MLocatorUDPInfo );

	return true;
}

void MLocator::Destroy()
{
	// MCommandCommunicator::Destroy();

	ReleaseDBMgr();
	ReleaseSafeUDP();
	ReleaseUDPManager();
	ReleaseServerStatusMgr();
	ReleaseServerStatusInfoBlob();
}


void MLocator::ReleaseDBMgr()
{
	if( 0 != m_pDBMgr )
	{
		m_pDBMgr->Disconnect();
		delete m_pDBMgr;
		m_pDBMgr = 0;
	}
}


void MLocator::ReleaseSafeUDP()
{
	if( 0 != m_pSafeUDP )
	{
		m_pSafeUDP->Destroy();
		delete m_pSafeUDP;
		m_pSafeUDP = 0;
	}
}


void MLocator::ReleaseServerStatusMgr()
{
	if( 0 != m_pServerStatusMgr )
	{
		delete m_pServerStatusMgr;
		m_pServerStatusMgr = 0;
	}
}


void MLocator::ReleaseServerStatusInfoBlob()
{
	if( 0 != m_vpServerStatusInfoBlob )
	{
		MEraseBlobArray( m_vpServerStatusInfoBlob );
		m_vpServerStatusInfoBlob = 0;
	}
}


void MLocator::ReleaseUDPManager()
{
	if( 0 != m_pRecvUDPManager )
	{
		m_pRecvUDPManager->SafeDestroy();
		delete m_pRecvUDPManager;
		m_pRecvUDPManager = 0;
	}

	if( 0 != m_pSendUDPManager )
	{
		m_pSendUDPManager->SafeDestroy();
		delete m_pSendUDPManager;
		m_pSendUDPManager = 0;
	}

	if( 0 != m_pBlockUDPManager )
	{
		m_pBlockUDPManager->SafeDestroy();
		delete m_pBlockUDPManager;
		m_pBlockUDPManager = 0;
	}

	ReleaseMemPool( MLocatorUDPInfo );

	UninitMemPool( MLocatorUDPInfo );
}



void MLocator::ReleaseCommand()
{
	while(MCommand* pCmd = GetCommandSafe())
		delete pCmd;
}


void MLocator::GetDBServerStatus( const DWORD dwEventTime, const bool bIsWithoutDelayUpdate )
{
	// 30초마다 DB의 ServerStatus테이블정보를 가져옴.

	if( IsElapedServerStatusUpdatedTime(dwEventTime) || bIsWithoutDelayUpdate )
	{
		if( (0 != GetLocatorDBMgr()) && (0 != m_pServerStatusMgr) )
		{
			m_pServerStatusMgr->Clear();
			if( GetLocatorDBMgr()->GetServerStatus(m_pServerStatusMgr) )
			{
				m_pServerStatusMgr->CheckDeadServerByLastUpdatedTime( GetLocatorConfig()->GetMarginOfErrorMin(), 
																	  m_pServerStatusMgr->CalcuMaxCmpCustomizeMin() );
				/*
				 * ServerStatusInfo Blob은 List수가 병경되었을 경우(Blob의 size가 변경)만 다시 할당을 하고,
				 *  그 외에는 할당된 메모리를 다시 사용함.
				 */

				if( m_nLastGetServerStatusCount != m_pServerStatusMgr->GetSize() )
				{
					MEraseBlobArray( m_vpServerStatusInfoBlob );

					m_nLastGetServerStatusCount = m_pServerStatusMgr->GetSize();
					m_vpServerStatusInfoBlob	= MMakeBlobArray( MTD_SERVER_STATUS_INFO_SIZE, m_nLastGetServerStatusCount );
					m_nServerStatusInfoBlobSize = MGetBlobArraySize( m_vpServerStatusInfoBlob );
				}
				if( 0 != m_vpServerStatusInfoBlob )
				{
					MTD_ServerStatusInfo* pMTDss;
					for( int i = 0; i < m_nLastGetServerStatusCount; ++i )
					{
						pMTDss = (MTD_ServerStatusInfo*)MGetBlobArrayElement( m_vpServerStatusInfoBlob, i );

						pMTDss->m_dwIP			= (*m_pServerStatusMgr)[i].GetIP();						
						pMTDss->m_nPort			= (*m_pServerStatusMgr)[i].GetPort();
						pMTDss->m_nServerID		= static_cast<unsigned char>( (*m_pServerStatusMgr)[i].GetID() );
						pMTDss->m_nCurPlayer	= (*m_pServerStatusMgr)[i].GetCurPlayer();
						pMTDss->m_nMaxPlayer	= (*m_pServerStatusMgr)[i].GetMaxPlayer();
						pMTDss->m_nType			= (*m_pServerStatusMgr)[i].GetType();
						pMTDss->m_bIsLive		= (*m_pServerStatusMgr)[i].IsLive();
#if defined(LOCALE_NHNUSA)
						pMTDss->m_dwAgentIP		= (*m_pServerStatusMgr)[i].GetAgentIP();
#endif
						strcpy ( pMTDss->m_szServerName	, (*m_pServerStatusMgr)[i].GetServerName().c_str() );

					}

					UpdateLastServerStatusUpdatedTime( dwEventTime );
				}
				else
				{
					m_nLastGetServerStatusCount = -1;
				}
			}
			else
			{
				mlog( "Fail to GetServerStatus\n" );
				ASSERT( 0 && "GetServerStatus실패." );
			}
		}
	}
}


bool MLocator::IsElapedServerStatusUpdatedTime( const DWORD dwEventTime )
{
	return (GetLocatorConfig()->GetMaxElapsedUpdateServerStatusTime() < (dwEventTime - GetUpdatedServerStatusTime()));
}


bool MLocator::IskLIveBlockUDP( const MLocatorUDPInfo* pBlkRecvUDPInfo, const DWORD dwEventTime )
{
	if( 0 == pBlkRecvUDPInfo ) return false;

	if( GetLocatorConfig()->GetBlockTime() > (dwEventTime - pBlkRecvUDPInfo->GetUseStartTime()) )
		return true;

	return false;
}


bool MLocator::IsBlocker( const DWORD dwIPKey, const DWORD dwEventTime )
{
	MUDPManager& rfBlkUDPMgr = GetBlockUDPManager();

	rfBlkUDPMgr.Lock();

	MLocatorUDPInfo* pBlkRecvUDPInfo = rfBlkUDPMgr.Find( dwIPKey );
	if( 0 != pBlkRecvUDPInfo )
	{
		// 현제 유저를 계속 블럭시킬지 결정.
		if( !IskLIveBlockUDP(pBlkRecvUDPInfo, dwEventTime) )
		{
			rfBlkUDPMgr.Delete( dwIPKey );
			rfBlkUDPMgr.Unlock();
			return false;
		}

		pBlkRecvUDPInfo->IncreaseUseCount();

		// 다시 허용치를 초과하면 블럭 시간을 연장함.
		if( IsOverflowedNormalUseCount(pBlkRecvUDPInfo) )
		{
			SYSTEMTIME st;
			// GetSystemTime(&st);
			GetLocalTime( &st );

			/*
			mlog( "IsBlocker - Reset block time. %u.%u.%u %u:%u:%u, dwIP:%u\n", 
				st.wYear, st.wMonth, st.wDay, st.wHour + 9, st.wMinute, st.wSecond, dwIPKey );
				*/
			
			pBlkRecvUDPInfo->SetUseStartTime( dwEventTime );
			pBlkRecvUDPInfo->SetUseCount( 1 );
		}

#ifdef _DEBUG
		mlog( "MLocator::IsBlocker - Block! time. dwIP:%u, UseCount:%d, LimitUseCount:%d, DbgInfo:%s\n",
			dwIPKey, 
			pBlkRecvUDPInfo->GetUseCount(), 
			GetLocatorConfig()->GetMaxFreeUseCountPerLiveTime(), 
			rfBlkUDPMgr.m_strExDbgInfo.c_str() );
#endif

		rfBlkUDPMgr.Unlock();
		return true;
	}

	rfBlkUDPMgr.Unlock();

	return false;
}// IsBlocker


bool MLocator::IsDuplicatedUDP( const DWORD dwIPKey, MUDPManager& rfCheckUDPManager, const DWORD dwEventTime )
{
	rfCheckUDPManager.Lock();

	MLocatorUDPInfo* pRecvUDPInfo = rfCheckUDPManager.Find( dwIPKey );
	if( 0 != pRecvUDPInfo )
	{
		pRecvUDPInfo->IncreaseUseCount();
		rfCheckUDPManager.Unlock();
		return false;
	}

	rfCheckUDPManager.Unlock();

	return true;
}


bool MLocator::UDPSocketRecvEvent( DWORD dwIP, WORD wRawPort, char* pPacket, DWORD dwSize)
{
	if( NULL == GetMainLocator() ) return false;
	if( sizeof(MPacketHeader) > dwSize ) return false;

	// 특정 아이피의 요청이 과도하게 오는지 필터링 함.
	// 필터링된 아이피는 블럭리스트에 등록되어 일정시간동안 요청이 무시됨.

	const DWORD dwRealIP = dwIP;

	const DWORD dwEventTime = timeGetTime(); // Recv이벤트처리에 사용할 시간.

	MLocator* pLocator = GetMainLocator();
	
	if( pLocator->IsBlocker(dwIP, dwEventTime ) ) 
		return false;
	
#ifdef _LOCATOR_TEST
	if( 400 < GetMainLocator()->GetRecvCount() ) 
		return true;
#endif
	
	if( !pLocator->IsDuplicatedUDP(dwIP, pLocator->GetRecvUDPManager(), dwEventTime) ) 
	{
		GetMainLocator()->IncreaseDuplicatedCount();
		return false;
	}
	
	// 여기까지 오면 새로운 UDP이므로 추가 작업을 함.

	MPacketHeader* pPacketHeader = (MPacketHeader*)pPacket;
	
	if ((dwSize != pPacketHeader->nSize) || 
		((pPacketHeader->nMsg != MSGID_COMMAND) && (pPacketHeader->nMsg != MSGID_RAWCOMMAND)) ) return false;
	
	unsigned int nPort = ntohs(wRawPort);
	GetMainLocator()->ParseUDPPacket( &pPacket[MPACKET_HEADER_SIZE], pPacketHeader, dwIP, nPort );

	GetMainLocator()->IncreaseRecvCount();

	_ASSERT(dwRealIP == dwIP);

	return true;
}// UDPSocketRecvEvent


void MLocator::ParseUDPPacket( char* pData, MPacketHeader* pPacketHeader, DWORD dwIP, unsigned int nPort )
{
	switch (pPacketHeader->nMsg)
	{
	case MSGID_RAWCOMMAND:
		{
			unsigned short nCheckSum = MBuildCheckSum(pPacketHeader, pPacketHeader->nSize);
			if (pPacketHeader->nCheckSum != nCheckSum) {
				static int nLogCount = 0;
				if (nLogCount++ < 100) {	// Log Flooding 방지
					mlog("MMatchClient::ParseUDPPacket() -> CHECKSUM ERROR(R=%u/C=%u)\n", 
						pPacketHeader->nCheckSum, nCheckSum);
				}
				return;
			} else {
				MCommand* pCmd = new MCommand();
				if (!pCmd->SetData(pData, &m_CommandManager))
				{
					char szLog[ 128 ] = {0,};
					_snprintf( szLog, 127, "MLocator::ParseUDPPacket -> SetData Error\n" );
					GetLogManager().SafeInsertLog( szLog );
					
					delete pCmd;
					return;
				}

				// 요청한 커맨드가 Locator에서 처리하는 건지 검사.
				if( MC_REQUEST_SERVER_LIST_INFO == pCmd->GetID() )
				{
					// 정보를 보내는데 필요한 DWORD IP만 큐에 저장. - 여기까지 와야 등록이 됨.
					if( !GetRecvUDPManager().SafeInsert(dwIP, nPort, timeGetTime()) )
					{
						char szLog[ 1024 ] = {0,};
						_snprintf( szLog, 1023, "fail to insert new IP(%u,%d) Time:%s\n", 
							dwIP, nPort, MGetStrLocalTime().c_str() );
						GetLogManager().SafeInsertLog( szLog );
					}
				}
				else
				{
					ASSERT( 0 && "현제 추가정의된 처리커맨드가 없음." );

					char szLog[ 1024 ] = {0,};
					_snprintf( szLog, 1023, "invalide command(%u) Time:%s, dwIP:%u\n", 
						pCmd->GetID(), MGetStrLocalTime().c_str(), dwIP );
					GetLogManager().SafeInsertLog( szLog );

					GetBlockUDPManager().SafeInsert( dwIP, nPort, timeGetTime() );
					GetLocatorStatistics().IncreaseBlockCount();
				}

				// 현제는 서버 상태정보 리스트만 요청하는 커맨드만 처리함.
				// MCommand는 사용하지 않음.
				delete pCmd;
			}
		}
		break;
	case MSGID_COMMAND:
		{
			ASSERT( 0 && "암호화 패킷 처리도 필요함." );
			char szLog[ 1024 ] = {0,};
			_snprintf( szLog, 1023, "encpypted command. Time:%s, dwIP:%u\n", 
				MGetStrLocalTime().c_str(), dwIP );
			GetLogManager().SafeInsertLog( szLog );

			// 계속되는 IP로그를 위해서 블럭리스트에 추가.
			GetBlockUDPManager().SafeInsert( dwIP, nPort, timeGetTime() );
			GetLocatorStatistics().IncreaseBlockCount();

			unsigned short nCheckSum = MBuildCheckSum(pPacketHeader, pPacketHeader->nSize);
			if (pPacketHeader->nCheckSum != nCheckSum) {
				static int nLogCount = 0;
				if (nLogCount++ < 100) {	// Log Flooding 방지
					mlog("MMatchClient::ParseUDPPacket() -> CHECKSUM ERROR(R=%u/C=%u)\n", 
						pPacketHeader->nCheckSum, nCheckSum);
				}
				return;
			} 
			else {
			}
		}
		break;
	default:
		{
			// MLog("MatchClient: Parse Packet Error");
			ASSERT( 0 && "정의도지 않은 타입." );
		}
		break;
	}
}// MLocator::ParseUDPPacket


void MLocator::Run()
{
	const DWORD dwEventTime = timeGetTime();
	GetDBServerStatus( dwEventTime );
	FlushRecvQueue( dwEventTime );
	UpdateUDPManager( dwEventTime );
	UpdateLocatorStatus( dwEventTime );
	UpdateLocatorLog( dwEventTime );
	UpdateLogManager();
}


void MLocator::ResponseServerStatusInfoList( DWORD dwIP, int nPort )
{
	if( 0 < m_nLastGetServerStatusCount )
	{
		MCommand* pCmd = CreateCommand( MC_RESPONSE_SERVER_LIST_INFO, MUID(0, 0) );
		if( 0 != pCmd )
		{
			// GetDBServerStatus에서 만들어진 정보를 보내줌.
			pCmd->AddParameter( new MCommandParameterBlob(m_vpServerStatusInfoBlob, m_nServerStatusInfoBlobSize) );
			SendCommandByUDP( dwIP, nPort, pCmd );
			delete pCmd;
		}
	}
}


bool MLocator::IsLiveUDP( const MLocatorUDPInfo* pRecvUDPInfo, const DWORD dwEventTime )
{
	if( 0 == pRecvUDPInfo ) return false;

	if( GetLocatorConfig()->GetUDPLiveTime() > (dwEventTime - pRecvUDPInfo->GetUseStartTime()) )
		return true;

	return false;
}


bool MLocator::IsOverflowedNormalUseCount( const MLocatorUDPInfo* pRecvUDPInfo )
{
	if( 0 == pRecvUDPInfo ) return false;

	if( GetLocatorConfig()->GetMaxFreeUseCountPerLiveTime() > pRecvUDPInfo->GetTotalUseCount() )
		return false;
	
	return true;
}


const int MLocator::MakeCmdPacket( char* pOutPacket, const int nMaxSize, MCommand* pCmd )
{
	if( (0 == pOutPacket) || (0 > nMaxSize) || (0 == pCmd) ) 
		return -1;

	MCommandMsg* pMsg = reinterpret_cast< MCommandMsg* >( pOutPacket );

	const int nCmdSize = nMaxSize - MPACKET_HEADER_SIZE;

	pMsg->Buffer[ 0 ] = 0;
	pMsg->nCheckSum = 0;

	if( pCmd->m_pCommandDesc->IsFlag(MCCT_NON_ENCRYPTED) )
	{
		pMsg->nMsg = MSGID_RAWCOMMAND;

		const int nGetCmdSize = pCmd->GetData( pMsg->Buffer, nCmdSize );
		if( nGetCmdSize != nCmdSize )
			return -1;

		pMsg->nSize		= static_cast< unsigned int >( MPACKET_HEADER_SIZE ) + nGetCmdSize;
		pMsg->nCheckSum = MBuildCheckSum(pMsg, pMsg->nSize);
	}
	else
	{
		ASSERT( 0 && "암호화된 커맨드 처리는 없음.\n" );
		return -1;
	}

	return pMsg->nSize;
}


void MLocator::SendCommandByUDP( DWORD dwIP, int nPort, MCommand* pCmd )
{
	const int nPacketSize = CalcPacketSize( pCmd );
	char* pszPacketBuf = new char[ nPacketSize ];
	if( 0 != pszPacketBuf ) 
	{
		const int nMakePacketSize = MakeCmdPacket( pszPacketBuf, nPacketSize, pCmd );
		if( nPacketSize == nMakePacketSize )
		{
			if( !m_pSafeUDP->Send(dwIP, nPort, pszPacketBuf, nMakePacketSize) )
			{
				delete [] pszPacketBuf;
				// mlog( "MLocator::SendCommandByUDP - fail:%u.\n", dwIP );
			}
		}
		else
		{
			delete [] pszPacketBuf;
			ASSERT( 0 && "Packet을 만드는데 문제가 있음." );
		}
	}
}


void MLocator::OnRegisterCommand(MCommandManager* pCommandManager)
{
	if( 0 != pCommandManager )
		MAddSharedCommandTable( pCommandManager, 0 );
}


void MLocator::UpdateUDPManager( const DWORD dwEventTime )
{
	if( GetLocatorConfig()->GetUpdateUDPManagerElapsedTime() < (dwEventTime - GetLastUDPManagerUpdateTime()) )
	{
		GetSendUDPManager().SafeClearElapsedLiveTimeUDP( GetLocatorConfig()->GetUpdateUDPManagerElapsedTime(), dwEventTime );
		GetBlockUDPManager().SafeClearElapsedLiveTimeUDP( GetLocatorConfig()->GetBlockTime(), dwEventTime );

		UpdateLastUDPManagerUpdateTime( dwEventTime );
	}
}


void MLocator::UpdateLocatorLog( const DWORD dwEventTime )
{
	if( GetLocatorConfig()->GetElapsedTimeUpdateLocatorLog() < (dwEventTime - GetLocatorStatistics().GetLastUpdatedTime()) )
	{
		GetLocatorDBMgr()->InsertLocatorLog( GetLocatorConfig()->GetLocatorID(),
			GetLocatorStatistics().GetCountryStatistics() );
		if( 0 != GetServerStatusMgr() )
		{
			GetLocatorStatistics().SetDeadServerCount( GetServerStatusMgr()->GetDeadServerCount() );
			GetLocatorStatistics().SetLiveServerCount( GetServerStatusMgr()->GetLiveServerCount() );
		}
		else
		{
			GetLocatorStatistics().SetDeadServerCount( 0 );
			GetLocatorStatistics().SetLiveServerCount( 0 );
		}
		GetLocatorStatistics().SetLastUpdatedTime( dwEventTime );
		GetLocatorStatistics().Reset();
	}
}


void MLocator::UpdateCountryCodeFilter( const DWORD dwEventTime )
{
	// 고려해봐야함.
	//if( 0 == GetLocatorDBMgr() ) return;

	//if( GetLocatorConfig()->GetElapsedTimeUpdateCountryCodeFilter() <
	//	(dwEventTime - GetCountryCodeFilter()->GetLastUpdatedTime()) )
	//{
	//	BlockCountryCodeInfoList	bcci;
	//	IPtoCountryList				icl;

	//	if( !GetLocatorDBMgr()->GetBlockCountryCodeList(&bcci) )
	//	{
	//		mlog( "Fail to GetDBBlockCountryCodeList.\n" );
	//		return;
	//	}

	//	if( !GetLocatorDBMgr()->GetIPtoCountryList(&icl) )
	//	{
	//		mlog( "Fail to GetDBIPtoCountryList.\n" );
	//		return;
	//	}

	//	IPtoCountryList::iterator it, end;
	//	end = icl.end();
	//	for( it = icl.begin(); it != end; ++it )
	//		GetLocatorStatistics().InitInsertCountryCode( it->strCountryCode3 ); // 국가의 접속 시도카운트를 위해서, 각 국가코드를 등록함.
	//	
	//	if( !GetCountryCodeFilter()->Update( bcci, icl) )
	//		mlog( "fail to update country code filter.\n" );

	//	GetCountryCodeFilter()->SetLastUpdatedTime( dwEventTime );
	//}
}


void MLocator::UpdateLogManager()
{
	// 다른 쓰레드에서 저장된 로그를 출력함.
	GetLogManager().SafeWriteMLog();
	GetLogManager().SafeReset();
}


void MLocator::FlushRecvQueue( const DWORD dwEventTime )
{
	MUDPManager& RecvUDPMgr = GetRecvUDPManager();
	MUDPManager& SendUDPMgr = GetSendUDPManager();

	MLocatorUDPInfo* pRecvUDPInfo;
	MLocatorUDPInfo* pSendUDPInfo;

	string	strCountryCode;
	string	strRoutingURL;
	bool	bIsBlock;
	string	strComment;

    RecvUDPMgr.Lock();
	while( 0 != (pRecvUDPInfo = RecvUDPMgr.SafePopFirst()) )
	{
		if( !SendUDPMgr.Insert(pRecvUDPInfo->GetIP(), pRecvUDPInfo) )
		{
			pSendUDPInfo = SendUDPMgr.Find( pRecvUDPInfo->GetIP() );
			if( 0 != pSendUDPInfo )
				pSendUDPInfo->IncreaseUseCount( pRecvUDPInfo->GetUseCount() );
			delete pRecvUDPInfo;
		}
	}
	RecvUDPMgr.Unlock();

	for( SendUDPMgr.SetBegin(); 0 != (pSendUDPInfo = SendUDPMgr.GetCurPosUDP()); )
	{
		if( 0 < pSendUDPInfo->GetUseCount() )
		{
			if( IsOverflowedNormalUseCount(pSendUDPInfo) )
			{
				// SendQ에 남아있는 BlockUDP는 전체UDPManager업데이트에서 일정 시간이 지나면 자동으로 지워짐.
				// SendUDPMgr.PopByIPKey( pSendUDPInfo->GetIP() );

				if( !IsBlocker(pSendUDPInfo->GetIP(), dwEventTime) )
				{
					pSendUDPInfo->SetUseCount( 0 );

					// 새로운 BlockUDP의 정보로 BlockQ에 등록함.
					if( !GetBlockUDPManager().SafeInsert(pSendUDPInfo->GetIP(), pSendUDPInfo->GetPort(), dwEventTime) )
					{
						mlog( "fail to block udp(%s) time:%s\n", 
							pSendUDPInfo->GetStrIP().c_str(), MGetStrLocalTime().c_str() );
					}

					GetLocatorStatistics().IncreaseBlockCount();
					mlog( "Block. IP(%s), time:%s\n", pSendUDPInfo->GetStrIP().c_str(), MGetStrLocalTime().c_str() );
				}
				SendUDPMgr.MoveNext();
				continue;
			}

			// 국가 코드 필터를 사용할시, 접속한 IP가 접속 가능한 국가인지 검사.

			string CountryCode3;

			ResponseServerStatusInfoList( pSendUDPInfo->GetIP(), pSendUDPInfo->GetPort() );

			GetLocatorStatistics().IncreaseCountryStatistics( CountryCode3 );


			pSendUDPInfo->IncreaseUsedCount( pSendUDPInfo->GetUseCount() );
			pSendUDPInfo->SetUseCount( 0 );
			IncreaseSendCount();

			
		}
		SendUDPMgr.MoveNext();
	}
}


MCommand* MLocator::CreateCommand(int nCmdID, const MUID& TargetUID)
{
	return new MCommand(m_CommandManager.GetCommandDescByID(nCmdID), TargetUID, m_This);
}


void MLocator::DumpLocatorStatusInfo()
{
	mlog( "\n======================================================\n" );
	mlog( "Locator Status Info.\n" );

	mlog( "Recv UDP Manager Status Info\n" );
	GetRecvUDPManager().Lock();
	GetRecvUDPManager().DumpStatusInfo();
	GetRecvUDPManager().Unlock();

	mlog( "Send UDP Manager Status Info\n" );
	GetSendUDPManager().Lock();
	GetSendUDPManager().DumpStatusInfo();
	GetSendUDPManager().Unlock();

	mlog( "Block UDP Manager Status Info\n" );
	GetBlockUDPManager().Lock();
	GetBlockUDPManager().DumpStatusInfo();
	GetBlockUDPManager().DumpUDPInfo();
	GetBlockUDPManager().Unlock();
	mlog( "======================================================\n\n" );
}


void MLocator::UpdateLocatorStatus( const DWORD dwEventTime )
{
	if( GetLocatorConfig()->GetMaxElapsedUpdateServerStatusTime() < 
		(dwEventTime - GetLastLocatorStatusUpdatedTime()) )
	{
		if( 0 == m_pDBMgr )
			return;

		if( !m_pDBMgr->UpdateLocaterStatus( GetLocatorConfig()->GetLocatorID(), 
			GetRecvCount(), 
			GetSendCount(), 
			static_cast<DWORD>(GetBlockUDPManager().size()), 
			GetDuplicatedCount() ) )
		{
			mlog( "fail to update locator status.\n" );
		}

		ResetRecvCount();
		ResetSendCount();
		ResetDuplicatedCount();

		UpdateLastLocatorStatusUpdatedTime( dwEventTime );
	}
}



//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#ifdef _DEBUG

// 초기화가 실패하더라도 실행에는 영향이 없어야 함.
void MLocator::InitDebug()
{
	GetRecvUDPManager().m_strExDbgInfo = "Name:RecvUDPManager";
	GetSendUDPManager().m_strExDbgInfo = "Name:SendUDPManager";
	GetBlockUDPManager().m_strExDbgInfo = "Name:BlockUDPManager";
}


void MLocator::TestDo()
{
	if( 0 != m_pDBMgr )
	{
		if( IsElapedServerStatusUpdatedTime(timeGetTime()) )
		{
			/*
			ServerStatusVec ss;
			if( !m_pDBMgr->GetServerStatus(ss) )
			{
				ASSERT( 0 );
			}

			for( int i = 0; i < ss.size(); ++i )
			{
				OutputDebugString( ss[i].GetDebugString().c_str() );
			}
			
			UpdateLastServerStatusUpdatedTime( timeGetTime() );
			*/
		}
	}
}

void MLocator::DebugOutput( void* vp )
{
	return; 

	MServerStatusMgr ssm = *( reinterpret_cast<MServerStatusMgr*>(vp) );

	int nSize = ssm.GetSize();

	char szBuf[ 1024 ];

	OutputDebugString( "\nStart Debug Output-------------------------------------------------\n" );

	for( int i = 0; i < nSize; ++i )
	{
		_snprintf( szBuf, 1023, "dwIP:%u, Port:%d, ServerID:%d, Time:%s, Live:%d (%d/%d)\n",
			ssm[i].GetIP(), ssm[i].GetPort(), ssm[i].GetID(), ssm[i].GetLastUpdatedTime().c_str(), 
			ssm[i].IsLive(), 
			ssm[i].GetCurPlayer(), ssm[i].GetMaxPlayer() );

		OutputDebugString( szBuf );

		char szVal[ 3 ];
		strncpy( szVal, &ssm[i].GetLastUpdatedTime()[11], 2 );
		const int nHour = atoi( szVal );
		strncpy( szVal, &ssm[i].GetLastUpdatedTime()[14], 2 );
		const int nMin = atoi( szVal );

		_snprintf( szBuf, 1023, "%d:%d\n", nHour, nMin );

		OutputDebugString( szBuf );
	}

	OutputDebugString( "End Debug Output---------------------------------------------------\n\n" );
}
#endif
