/*******************************************************************************


   Copyright (C) 2011-2014 SequoiaDB Ltd.

   This program is free software: you can redistribute it and/or modify
   it under the term of the GNU Affero General Public License, version 3,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warrenty of
   MARCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program. If not, see <http://www.gnu.org/license/>.

   Source File Name = clsShardSession.cpp

   Dependencies: N/A

   Restrictions: N/A

   Change Activity:
   defect Date        Who Description
   ====== =========== === ==============================================
          07/12/2012  Xu Jianhui  Initial Draft

   Last Changed =

*******************************************************************************/

#include "clsShardSession.hpp"
#include "pmd.hpp"
#include "clsMgr.hpp"
#include "msgMessage.hpp"
#include "pdTrace.hpp"
#include "clsTrace.hpp"
#include "rtnDataSet.hpp"
#include "rtnContextShdOfLob.hpp"
#include "utilCompressor.hpp"

using namespace bson ;

namespace engine
{

#define SHD_SESSION_TIMEOUT         (60)
#define SHD_INTERRUPT_CHECKPOINT    (10)
#define SHD_NOTPRIMARY_WAITTIME     (15000)     //ms
#define SHD_TRANSROLLBACK_WAITTIME  (600000)    //ms
#define SHD_WAITTIME_INTERVAL       (200)       //ms

   BEGIN_OBJ_MSG_MAP( _clsShdSession, _pmdAsyncSession )
      ON_MSG ( MSG_BS_UPDATE_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_INSERT_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_DELETE_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_QUERY_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_GETMORE_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_KILL_CONTEXT_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_MSG_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_INTERRUPTE, _onOPMsg )
      ON_MSG ( MSG_BS_TRANS_BEGIN_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_TRANS_COMMIT_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_TRANS_ROLLBACK_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_TRANS_COMMITPRE_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_TRANS_UPDATE_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_TRANS_DELETE_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_TRANS_INSERT_REQ, _onOPMsg )
      ON_MSG ( MSG_COM_SESSION_INIT_REQ, _onOPMsg )
#if defined (_DEBUG)
      ON_MSG ( MSG_AUTH_VERIFY_REQ, _onOPMsg )
      ON_MSG ( MSG_AUTH_CRTUSR_REQ, _onOPMsg )
      ON_MSG ( MSG_AUTH_DELUSR_REQ, _onOPMsg )
#endif
      ON_MSG ( MSG_BS_LOB_OPEN_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_LOB_WRITE_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_LOB_READ_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_LOB_CLOSE_REQ, _onOPMsg )
      ON_MSG ( MSG_BS_LOB_REMOVE_REQ, _onOPMsg )
      ON_MSG ( MSG_CAT_GRP_CHANGE_NTY, _onCatalogChangeNtyMsg )

      ON_EVENT( PMD_EDU_EVENT_TRANS_STOP, _onTransStopEvnt )
   END_OBJ_MSG_MAP()

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSDSESS__CLSSHDSESS, "_clsShdSession::_clsShdSession" )
   _clsShdSession::_clsShdSession ( UINT64 sessionID )
      :_pmdAsyncSession ( sessionID )
   {
      PD_TRACE_ENTRY ( SDB__CLSSDSESS__CLSSHDSESS ) ;
      _pCollectionName  = NULL ;
      _isMainCL         = FALSE ;
      _hasUpdateCataInfo= FALSE ;
      pmdKRCB *pKRCB = pmdGetKRCB () ;
      _pReplSet  = sdbGetReplCB () ;
      _pShdMgr   = sdbGetShardCB () ;
      _pCatAgent = pKRCB->getClsCB ()->getCatAgent () ;
      _pDmsCB    = pKRCB->getDMSCB () ;
      _pDpsCB    = pKRCB->getDPSCB () ;
      _pRtnCB    = pKRCB->getRTNCB () ;
      _primaryID.value = MSG_INVALID_ROUTEID ;
      ossMemset( _detailName, 0, sizeof( _detailName ) ) ;
      _logout    = TRUE ;
      _delayLogin= FALSE ;
      PD_TRACE_EXIT ( SDB__CLSSDSESS__CLSSHDSESS ) ;
   }

   _clsShdSession::~_clsShdSession ()
   {
      _pReplSet  = NULL ;
      _pShdMgr   = NULL ;
      _pCatAgent = NULL ;
      _pDmsCB    = NULL ;
      _pRtnCB    = NULL ;
      _pDpsCB    = NULL ;
      _pCollectionName = NULL ;
      _cmdCollectionName.clear() ;
   }

   const CHAR* _clsShdSession::sessionName() const
   {
      if ( 0 != _detailName[0] )
      {
         return _detailName ;
      }
      return _pmdAsyncSession::sessionName() ;
   }

   void _clsShdSession::clear()
   {
      ossMemset( _detailName, 0, sizeof( _detailName ) ) ;
      _username = "" ;
      _passwd = "" ;
      _logout = TRUE ;
      _delayLogin = FALSE ;
      _pmdAsyncSession::clear() ;
   }

   SDB_SESSION_TYPE _clsShdSession::sessionType() const
   {
      return SDB_SESSION_SHARD ;
   }

   EDU_TYPES _clsShdSession::eduType () const
   {
      return EDU_TYPE_SHARDAGENT ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS_ONRV, "_clsShdSession::onRecieve" )
   void _clsShdSession::onRecieve ( const NET_HANDLE netHandle,
                                    MsgHeader * msg )
   {
      PD_TRACE_ENTRY ( SDB__CLSSHDSESS_ONRV ) ;
      ossGetCurrentTime( _lastRecvTime ) ;
      PD_TRACE_EXIT ( SDB__CLSSHDSESS_ONRV ) ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS_TMOUT, "_clsShdSession::timeout" )
   BOOLEAN _clsShdSession::timeout ( UINT32 interval )
   {
      PD_TRACE_ENTRY ( SDB__CLSSHDSESS_TMOUT ) ;
      BOOLEAN ret = FALSE ;
      ossTimestamp curTime ;
      ossGetCurrentTime ( curTime ) ;

      if ( curTime.time - _lastRecvTime.time > SHD_SESSION_TIMEOUT &&
           _pEDUCB->contextNum() == 0 &&
           ( _pEDUCB->getTransID() == DPS_INVALID_TRANS_ID ||
           !(sdbGetReplCB()->primaryIsMe())))
      {
         // will be release
         ret = TRUE ;
         goto done ;
      }
   done :
      PD_TRACE_EXIT ( SDB__CLSSHDSESS_TMOUT ) ;
      return ret ;
   }

   BOOLEAN _clsShdSession::isSetLogout() const
   {
      return _logout ;
   }

   BOOLEAN _clsShdSession::isDelayLogin() const
   {
      return _delayLogin ;
   }

   void _clsShdSession::setLogout()
   {
      _logout = TRUE ;
   }

   void _clsShdSession::setDelayLogin( const clsIdentifyInfo &info )
   {
      _delayLogin = TRUE ;

      UINT32 ip = 0, port = 0 ;
      ossUnpack32From64( info._id, ip, port ) ;
      setIdentifyInfo( ip, port, info._tid, info._eduid ) ;
      _username = info._username ;
      _passwd = info._passwd ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__ONDETACH, "_clsShdSession::_onDetach" )
   void _clsShdSession::_onDetach ()
   {
      PD_TRACE_ENTRY ( SDB__CLSSHDSESS__ONDETACH ) ;
      if ( _pEDUCB )
      {
         INT64 contextID = -1 ;
         while ( -1 != ( contextID = _pEDUCB->contextPeek() ) )
         {
            _pRtnCB->contextDelete ( contextID, NULL ) ;
         }

         INT32 rcTmp = rtnTransRollback( _pEDUCB, _pDpsCB ) ;
         if ( rcTmp)
         {
            PD_LOG ( PDERROR, "Failed to rollback(rc=%d)", rcTmp ) ;
         }
      }

      /// has session init
      if ( 0 != _detailName[0] )
      {
         UINT32 ip = 0 ;
         UINT32 port = 0 ;
         ossUnpack32From64( identifyID(), ip, port ) ;

         /// audit
         CHAR szTmpIP[ 50 ] = { 0 } ;
         ossIP2Str( ip, szTmpIP, sizeof(szTmpIP) - 1 ) ;
         CHAR szTmpID[ 20 ] = { 0 } ;
         ossSnprintf( szTmpID, sizeof(szTmpID) - 1, "%llu", eduID() ) ;

         PD_AUDIT( AUDIT_ACCESS, _client.getUsername(), szTmpIP, (UINT16)port,
                   "LOGOUT", AUDIT_OBJ_SESSION, szTmpID, SDB_OK,
                   "User[UserName:%s, FromIP:%s, FromPort:%u, "
                   "FromSession:%llu, FromTID:%u] logout succeed",
                   _client.getUsername(), szTmpIP, port,
                   identifyEDUID(), identifyTID() ) ;
      }

      _pmdAsyncSession::_onDetach () ;
      PD_TRACE_EXIT ( SDB__CLSSHDSESS__ONDETACH ) ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__DFMSGFUNC, "_clsShdSession::_defaultMsgFunc" )
   INT32 _clsShdSession::_defaultMsgFunc ( NET_HANDLE handle, MsgHeader * msg )
   {
      INT32 rc = SDB_OK ;
      PD_TRACE_ENTRY ( SDB__CLSSHDSESS__DFMSGFUNC ) ;
      rc = _onOPMsg( handle, msg ) ;
      PD_TRACE_EXITRC ( SDB__CLSSHDSESS__DFMSGFUNC, rc ) ;
      return rc ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__REPLY, "_clsShdSession::_reply" )
   INT32 _clsShdSession::_reply ( MsgOpReply * header, const CHAR * buff,
                                  UINT32 size )
   {
      INT32 rc = SDB_OK ;
      PD_TRACE_ENTRY ( SDB__CLSSHDSESS__REPLY ) ;

      if ( (UINT32)(header->header.messageLength) !=
           sizeof (MsgOpReply) + size )
      {
         PD_LOG ( PDERROR, "Session[%s] reply message length error[%u != %u]",
                  sessionName() ,header->header.messageLength,
                  sizeof ( MsgOpReply ) + size ) ;
         rc = SDB_SYS ;
         goto error ;
      }

      //Send message
      if ( size > 0 )
      {
         rc = routeAgent()->syncSend ( _netHandle, (MsgHeader *)header,
                                       (void*)buff, size ) ;
      }
      else
      {
         rc = routeAgent()->syncSend ( _netHandle, (void *)header ) ;
      }

      if ( rc != SDB_OK )
      {
         PD_LOG ( PDERROR, "Session[%s] send reply message failed[rc:%d]",
            sessionName(), rc ) ;
      }
   done:
      PD_TRACE_EXITRC ( SDB__CLSSHDSESS__REPLY, rc ) ;
      return rc ;
   error:
      goto done ;
   }

   //message fuctions
   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__ONOPMSG, "_clsShdSession::_onOPMsg" )
   INT32 _clsShdSession::_onOPMsg ( NET_HANDLE handle, MsgHeader * msg )
   {
      PD_TRACE_ENTRY ( SDB__CLSSHDSESS__ONOPMSG ) ;
      BOOLEAN loop = TRUE ;
      INT32 loopTime = 0 ;
      INT32 rc = SDB_OK ;

      SINT64 contextID = -1 ;
      INT32 startFrom = 0 ;
      rtnContextBuf buffObj ;
      _pCollectionName = NULL ;
      _cmdCollectionName.clear() ;
      _isMainCL        = FALSE ;
      _hasUpdateCataInfo = FALSE ;
      BOOLEAN isNeedRollback = FALSE;

      _primaryID.value = MSG_INVALID_ROUTEID ;

      if ( isDelayLogin() )
      {
         _login() ;
      }

      while ( loop )
      {
         MON_START_OP( _pEDUCB->getMonAppCB() ) ;
         _pEDUCB->getMonAppCB()->setLastOpType( msg->opCode ) ;

         switch ( msg->opCode )
         {
            case MSG_BS_UPDATE_REQ :
               isNeedRollback = TRUE ;
               rc = _onUpdateReqMsg ( handle, msg, contextID ) ;
               break ;
            case MSG_BS_INSERT_REQ :
               {
               MsgOpInsert *pInsert = (MsgOpInsert*)msg ;
               INT32 insertedNum = 0 ;
               INT32 ignoredNum = 0 ;
               isNeedRollback = TRUE ;
               rc = _onInsertReqMsg ( handle, msg, insertedNum, ignoredNum ) ;
               if ( pInsert->flags & FLG_INSERT_RETURNNUM )
               {
                  contextID = ossPack32To64( insertedNum, ignoredNum ) ;
               }
               }
               break ;
            case MSG_BS_DELETE_REQ :
               isNeedRollback = TRUE ;
               rc = _onDeleteReqMsg ( handle, msg, contextID ) ;
               break ;
            case MSG_BS_QUERY_REQ :
               rc = _onQueryReqMsg ( handle, msg, buffObj, startFrom,
                                     contextID ) ;
               break ;
            case MSG_BS_GETMORE_REQ :
               rc = _onGetMoreReqMsg ( msg, buffObj, startFrom, contextID ) ;
               break ;
            case MSG_BS_TRANS_UPDATE_REQ :
               isNeedRollback = TRUE ;
               rc = _onTransUpdateReqMsg ( handle, msg, contextID ) ;
               break ;
            case MSG_BS_TRANS_INSERT_REQ :
               {
               INT32 insertedNum = 0 ;
               INT32 ignoredNum = 0 ;
               MsgOpInsert *pInsert = (MsgOpInsert*)msg ;
               isNeedRollback = TRUE ;
               rc = _onTransInsertReqMsg ( handle, msg, insertedNum,
                                           ignoredNum ) ;
               if ( pInsert->flags & FLG_INSERT_RETURNNUM )
               {
                  contextID = ossPack32To64( insertedNum, ignoredNum ) ;
               }
               }
               break ;
            case MSG_BS_TRANS_DELETE_REQ :
               isNeedRollback = TRUE ;
               rc = _onTransDeleteReqMsg ( handle, msg, contextID ) ;
               break ;
            case MSG_BS_KILL_CONTEXT_REQ :
               rc = _onKillContextsReqMsg ( handle, msg ) ;
               break ;
            case MSG_BS_MSG_REQ :
               rc = _onMsgReq ( handle, msg ) ;
               break ;
            case MSG_BS_INTERRUPTE :
               rc = _onInterruptMsg( handle, msg ) ;
               break ;
#if defined (_DEBUG)
            // for authentication message through sharding port, we simply
            // return OK
            case MSG_AUTH_VERIFY_REQ :
            case MSG_AUTH_CRTUSR_REQ :
            case MSG_AUTH_DELUSR_REQ :
               rc = SDB_OK ;
               break ;
#endif
            case MSG_BS_TRANS_BEGIN_REQ :
               rc = _onTransBeginMsg();
               break;

            case MSG_BS_TRANS_COMMIT_REQ :
               isNeedRollback = TRUE ;
               rc = _onTransCommitMsg();
               break;

            case MSG_BS_TRANS_ROLLBACK_REQ:
               rc = _onTransRollbackMsg();
               break;

            case MSG_BS_TRANS_COMMITPRE_REQ:
               rc = _onTransCommitPreMsg( msg );
               break;

            case MSG_COM_SESSION_INIT_REQ:
               rc = _onSessionInitReqMsg( msg );
               break;

            case MSG_BS_LOB_OPEN_REQ:
               rc = _onOpenLobReq( msg, contextID,
                                   buffObj ) ;
               break ;
            case MSG_BS_LOB_WRITE_REQ:
               rc = _onWriteLobReq( msg ) ;
               break ;
            case MSG_BS_LOB_READ_REQ:
               rc = _onReadLobReq( msg, buffObj ) ;
               break ;
            case MSG_BS_LOB_CLOSE_REQ:
               rc = _onCloseLobReq( msg ) ;
               break ;
            case MSG_BS_LOB_REMOVE_REQ:
               rc = _onRemoveLobReq( msg ) ;
               break ;
            case MSG_BS_LOB_UPDATE_REQ:
               rc = _onUpdateLobReq( msg ) ;
               break ;
            default:
               rc = SDB_CLS_UNKNOW_MSG ;
               break ;
         }

         //Need to update catalog info
         // SDB_CLS_NO_CATALOG_INFO: between update and check, this cata
         // will be removed by others, so need to retry all the way
         if ( SDB_CLS_NO_CATALOG_INFO == rc ||
              ( ( SDB_CLS_DATA_NODE_CAT_VER_OLD == rc ||
                  SDB_CLS_COORD_NODE_CAT_VER_OLD == rc
                 ) && loopTime < 1
               )
             )
         {
            loopTime++ ;
            PD_LOG ( PDWARNING, "Catalog is empty or older[rc:%d] in "
                     "session[%s]", rc, sessionName() ) ;
            rc = _pShdMgr->syncUpdateCatalog( _pCollectionName ) ;
            if ( SDB_OK == rc )
            {
               _hasUpdateCataInfo = TRUE ;
               continue ;
            }
         }
         //catalog has the collection, so need to create, no compression
         else if ( (SDB_DMS_CS_NOTEXIST == rc || SDB_DMS_NOTEXIST == rc) &&
                   _pCollectionName && _pReplSet->primaryIsMe() )
         {
            /// if main collection, need update catalog info first
            if ( _isMainCL )
            {
               if ( !_hasUpdateCataInfo )
               {
                  rc = _pShdMgr->syncUpdateCatalog( _pCollectionName ) ;
                  if ( SDB_OK == rc )
                  {
                     ++loopTime ;
                     _hasUpdateCataInfo = TRUE ;
                  }
               }
            }
            else if ( SDB_DMS_CS_NOTEXIST == rc )
            {
               rc = _createCSByCatalog( _pCollectionName ) ;
            }
            else if ( SDB_DMS_NOTEXIST == rc )
            {
               rc = _createCLByCatalog( _pCollectionName ) ;
            }

            if ( SDB_OK == rc )
            {
               continue ;
            }
         }

         if ( SDB_CLS_DATA_NODE_CAT_VER_OLD == rc )
         {
            rc = SDB_CLS_COORD_NODE_CAT_VER_OLD ;
         }

         loop = FALSE ;
      }

      if ( MSG_BS_INTERRUPTE == msg->opCode )
      {
         //not to reply
         goto done ;
      }

      if ( rc < -SDB_MAX_ERROR || rc > SDB_MAX_WARNING )
      {
         PD_LOG ( PDERROR, "Session[%s] OP[type:%u] return code error[rc:%d]",
                  sessionName(), msg->opCode, rc ) ;
         rc = SDB_SYS ;
      }

      //Build reply message
      _replyHeader.header.opCode = MAKE_REPLY_TYPE( msg->opCode ) ;
      _replyHeader.header.messageLength = sizeof ( MsgOpReply ) ;
      _replyHeader.header.requestID = msg->requestID ;
      _replyHeader.header.TID = msg->TID ;
      _replyHeader.header.routeID.value = 0 ;

      if ( SDB_OK != rc )
      {
         /// when coord catalog info is old, can't rollback, coord will retry
         if ( SDB_CLS_COORD_NODE_CAT_VER_OLD != rc &&
              isNeedRollback && _pReplSet->primaryIsMe () )
         {
            INT32 rcTmp = rtnTransRollback( _pEDUCB, _pDpsCB ) ;
            if ( rcTmp )
            {
               PD_LOG ( PDERROR, "Failed to rollback(rc=%d)", rcTmp ) ;
            }
         }

         if ( 0 == buffObj.size() )
         {
            _errorInfo = utilGetErrorBson( rc, _pEDUCB->getInfo(
                                           EDU_INFO_ERROR ) ) ;
            buffObj = rtnContextBuf( _errorInfo.objdata(),
                                     _errorInfo.objsize(),
                                     1 ) ;
         }

         if ( rc != SDB_DMS_EOC )
         {
            PD_LOG ( PDERROR, "Session[%s] process OP[type:%u] failed[rc:%d]",
                     sessionName(), msg->opCode, rc ) ;
         }

         if ( SDB_CLS_NOT_PRIMARY == rc && 0 != _primaryID.columns.nodeID )
         {
            // retrun the node id by startFrom
            startFrom = _primaryID.columns.nodeID ;
         }
      }

      _replyHeader.header.messageLength += buffObj.size() ;
      _replyHeader.flags = rc ;
      _replyHeader.contextID = contextID ;
      _replyHeader.numReturned = buffObj.recordNum() ;
      _replyHeader.startFrom = startFrom ;

      rc = _reply ( &_replyHeader, buffObj.data(), buffObj.size() ) ;

   done:
      eduCB()->writingDB( FALSE ) ;
      MON_END_OP( _pEDUCB->getMonAppCB() ) ;
      PD_TRACE_EXITRC ( SDB__CLSSHDSESS__ONOPMSG, rc ) ;
      return rc ;
   }

   INT32 _clsShdSession::_createCSByCatalog( const CHAR * clFullName )
   {
      INT32 rc = SDB_OK ;
      CHAR csName[ DMS_COLLECTION_SPACE_NAME_SZ + 1 ] = { 0 } ;
      INT32 index = 0 ;
      while ( clFullName[ index ] && index < DMS_COLLECTION_SPACE_NAME_SZ )
      {
         if ( '.' == clFullName[ index ] )
         {
            break ;
         }
         csName[ index ] = clFullName[ index ] ;
         ++index ;
      }

      UINT32 pageSize = DMS_PAGE_SIZE_DFT ;
      UINT32 lobPageSize = DMS_DEFAULT_LOB_PAGE_SZ ;
      rc = _pShdMgr->rGetCSPageSize( csName, pageSize, lobPageSize ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "Session[%s]: Get collection space[%s] page "
                 "size from catalog failed, rc: %d", sessionName(),
                 csName, rc ) ;
         goto error ;
      }
      rc = rtnCreateCollectionSpaceCommand( csName, _pEDUCB, _pDmsCB, _pDpsCB,
                                            pageSize, lobPageSize, FALSE ) ;
      if ( SDB_DMS_CS_EXIST == rc )
      {
         rc = SDB_OK ;
      }
      else if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "Session[%s]: Create collection space[%s] by "
                 "catalog failed, rc: %d", sessionName(), csName, rc ) ;
      }
      else
      {
         PD_LOG( PDEVENT, "Session[%s]: Create collection space[%s] by "
                 "catalog succeed", sessionName(), csName ) ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _clsShdSession::_createCLByCatalog( const CHAR *clFullName,
                                             const CHAR *pParent,
                                             BOOLEAN mustOnSelf )
   {
      INT32 rc                = SDB_OK ;
      UINT32 attribute        = 0 ;
      BOOLEAN isMainCL        = FALSE;
      UINT32 groupCount       = 0 ;
      BSONObj shardingKey ;
      vector< string > subCLList ;

      /// get sharding key
   retry:
      _pCatAgent->lock_r() ;
      clsCatalogSet *set = _pCatAgent->collectionSet( clFullName ) ;
      if ( NULL == set )
      {
         _pCatAgent->release_r() ;

         rc = _pShdMgr->syncUpdateCatalog( clFullName ) ;
         if ( SDB_OK == rc )
         {
            goto retry ;
         }
         else
         {
            PD_LOG( PDERROR, "Session[%s] Update collection[%s]'s "
                    "catalog info failed, rc: %d", sessionName(),
                    clFullName, rc ) ;
            goto error ;
         }
      }

      if ( set->isSharding() && set->ensureShardingIndex() )
      {
         shardingKey = set->getShardingKey().getOwned() ;
      }

      attribute = set->getAttribute() ;
      isMainCL = set->isMainCL() ;
      groupCount = set->groupCount() ;

      if ( isMainCL )
      {
         set->getSubCLList( subCLList ) ;
      }

      _pCatAgent->release_r() ;

      if ( isMainCL )
      {
         vector< string >::iterator iter = subCLList.begin() ;
         while ( iter != subCLList.end() )
         {
            rc = _createCLByCatalog( (*iter).c_str(), clFullName, FALSE ) ;
            if ( rc )
            {
               break ;
            }
            ++iter ;
         }
      }
      else
      {
         if( 0 == groupCount )
         {
            /// first clear
            _pCatAgent->lock_w() ;
            _pCatAgent->clear( clFullName ) ;
            _pCatAgent->release_w() ;

            if ( pParent && FALSE == mustOnSelf )
            {
               /// ignore this sub-collection
               goto done ;
            }

            rc = SDB_CLS_COORD_NODE_CAT_VER_OLD ;
            PD_LOG( PDERROR, "Session[%s]: Collection[%s] is not on this group",
                    sessionName(), clFullName ) ;
            goto error ;
         }

         rc = rtnCreateCollectionCommand( clFullName, shardingKey, attribute,
                                          _pEDUCB, _pDmsCB, _pDpsCB,
                                          UTIL_COMPRESSOR_INVALID,
                                          0, FALSE ) ;
         if ( SDB_DMS_EXIST == rc )
         {
            rc = SDB_OK ;
         }
         else if ( SDB_OK != rc )
         {
            if ( NULL == pParent )
            {
               PD_LOG( PDWARNING, "Session[%s]: Create collection[%s] by "
                       "catalog failed, rc: %d", sessionName(), clFullName,
                       rc ) ;
            }
            else
            {
               PD_LOG( PDWARNING, "Session[%s]: Create sub-collection[%] "
                       "of main-collection[%s] by catalog failed, rc: %d",
                       sessionName(), clFullName, pParent, rc ) ;
            }
         }
         else
         {
            if ( NULL == pParent )
            {
               PD_LOG( PDEVENT, "Session[%s]: Create collection[%s] by "
                       "catalog succeed", sessionName(), clFullName ) ;
            }
            else
            {
               PD_LOG( PDEVENT, "Session[%s]: Create sub-collection[%s] "
                       "of main-collection[%s] by catalog succeed",
                       sessionName(), clFullName, pParent ) ;
            }
         }
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _clsShdSession::_processSubCLResult( INT32 result,
                                              const CHAR *clFullName,
                                              const CHAR *pParent )
   {
      INT32 rc = SDB_OK ;

      if ( SDB_OK == result )
      {
         goto done ;
      }
      else if ( ( SDB_DMS_CS_NOTEXIST == result ||
                  SDB_DMS_NOTEXIST == result ) && pmdIsPrimary() )
      {
         if ( SDB_DMS_CS_NOTEXIST == result )
         {
            rc = _createCSByCatalog( clFullName ) ;
         }
         else
         {
            rc = _createCLByCatalog( clFullName, pParent ) ;
         }
      }
      else
      {
         rc = result ;
      }

   done:
      return rc ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__ONUPREQMSG, "_clsShdSession::_onUpdateReqMsg" )
   INT32 _clsShdSession::_onUpdateReqMsg( NET_HANDLE handle, MsgHeader * msg,
                                          INT64 &updateNum )
   {
      PD_TRACE_ENTRY ( SDB__CLSSHDSESS__ONUPREQMSG ) ;
      PD_LOG ( PDDEBUG, "session[%s] _onUpdateReqMsg", sessionName() ) ;

      INT32 rc = SDB_OK ;
      MsgOpUpdate *pUpdate = (MsgOpUpdate*)msg ;
      INT32 flags = 0 ;
      CHAR *pCollectionName = NULL ;
      CHAR *pSelectorBuffer = NULL ;
      CHAR *pUpdatorBuffer = NULL ;
      CHAR *pHintBuffer = NULL ;
      INT16 w = 0 ;
      INT16 clientW = pUpdate->w ;
      INT16 replSize = 0 ;

      rc = _checkWriteStatus() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "failed to check write status:%d", rc ) ;
         goto error ;
      }

      rc = msgExtractUpdate( (CHAR*)msg, &flags, &pCollectionName,
                             &pSelectorBuffer, &pUpdatorBuffer, &pHintBuffer );
      if ( SDB_OK != rc )
      {
         PD_LOG ( PDERROR, "Extract update message failed[rc:%d] in "
                  "session[%s]", rc, sessionName() ) ;
         goto error ;
      }
      _pCollectionName = pCollectionName ;

      rc = _checkCLStatusAndGetSth( pCollectionName,
                                    pUpdate->version,
                                    &_isMainCL, &replSize ) ;
      if ( SDB_OK != rc )
      {
         goto error ;
      }

      rc = _calculateW( &replSize, &clientW, w ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to calculate w:%d", rc ) ;
         goto error ;
      }

      try
      {
         BSONObj selector( pSelectorBuffer );
         BSONObj updator( pUpdatorBuffer );
         BSONObj hint( pHintBuffer );
         // add last op info
         MON_SAVE_OP_DETAIL( eduCB()->getMonAppCB(), msg->opCode,
                             "Collection:%s, Matcher:%s, Updator:%s, Hint:%s, "
                             "Flag:0x%08x(%u)",
                             pCollectionName,
                             selector.toString().c_str(),
                             updator.toString().c_str(),
                             hint.toString().c_str(),
                             flags, flags ) ;

         PD_LOG ( PDDEBUG, "Session[%s] Update: selctor: %s\nupdator: %s\n"
                  "hint: %s", sessionName(), selector.toString().c_str(),
                  updator.toString().c_str(), hint.toString().c_str() ) ;
         if ( _isMainCL )
         {
            rc = _updateToMainCL( pCollectionName, selector, updator, hint,
                                  flags, _pEDUCB, _pDmsCB, _pDpsCB, w,
                                  ( pUpdate->flags & FLG_UPDATE_RETURNNUM ) ?
                                  &updateNum : NULL );
         }
         else
         {
            rc = rtnUpdate( pCollectionName, selector, updator, hint,
                            flags, _pEDUCB, _pDmsCB, _pDpsCB, w,
                            ( pUpdate->flags & FLG_UPDATE_RETURNNUM ) ?
                            &updateNum : NULL ) ;
         }
      }
      catch ( std::exception &e )
      {
         PD_LOG ( PDERROR, "Session[%s] Failed to create selector and updator "
                  "for update: %s", sessionName(), e.what () ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }

   done:
      PD_TRACE_EXITRC ( SDB__CLSSHDSESS__ONUPREQMSG, rc ) ;
      return rc ;
   error:
      goto done ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__ONINSTREQMSG, "_clsShdSession::_onInsertReqMsg" )
   INT32 _clsShdSession::_onInsertReqMsg ( NET_HANDLE handle, MsgHeader * msg,
                                           INT32 &insertedNum,
                                           INT32 &ignoredNum )
   {
      PD_LOG ( PDDEBUG, "session[%s] _onInsertReqMsg", sessionName() ) ;

      INT32 rc = SDB_OK ;
      PD_TRACE_ENTRY ( SDB__CLSSHDSESS__ONINSTREQMSG ) ;
      INT32 flags = 0 ;
      CHAR *pCollectionName = NULL ;
      CHAR *pInsertorBuffer = NULL ;
      INT32 recordNum = 0 ;
      MsgOpInsert *pInsert = (MsgOpInsert*)msg ;
      INT16 w = 0 ;
      INT16 clientW = pInsert->w ;
      INT16 replSize = 0 ;

      rc = _checkWriteStatus() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "failed to check write status:%d", rc ) ;
         goto error ;
      }

      rc = msgExtractInsert ( (CHAR*)msg,  &flags, &pCollectionName,
                              &pInsertorBuffer, recordNum ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG ( PDERROR, "Session[%s] extract insert msg failed[rc:%d]",
                  sessionName(), rc ) ;
         goto error ;
      }
      _pCollectionName = pCollectionName ;

      rc = _checkCLStatusAndGetSth( pCollectionName,
                                    pInsert->version,
                                    &_isMainCL, &replSize ) ;
      if ( SDB_OK != rc )
      {
         goto error ;
      }

      rc = _calculateW( &replSize, &clientW, w ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to calculate w:%d", rc ) ;
         goto error ;
      }
      try
      {
         BSONObj insertor ( pInsertorBuffer ) ;
         // add list op info
         MON_SAVE_OP_DETAIL( eduCB()->getMonAppCB(), msg->opCode,
                             "Collection:%s, Insertors:%s, ObjNum:%d, "
                             "Flag:0x%08x(%u)",
                             pCollectionName,
                             insertor.toString().c_str(),
                             recordNum, flags, flags ) ;

         PD_LOG ( PDDEBUG, "Session[%s] Insert: %s\nCollection: %s",
                  sessionName(), insertor.toString().c_str(),
                  pCollectionName ) ;

         if ( _isMainCL )
         {
            rc = _insertToMainCL( insertor, recordNum, flags, w,
                                  insertedNum, ignoredNum );
         }
         else
         {

            rc = rtnInsert ( pCollectionName, insertor, recordNum, flags,
                             _pEDUCB, _pDmsCB, _pDpsCB, w,
                             &insertedNum, &ignoredNum ) ;
         }
      }
      catch ( std::exception &e )
      {
         PD_LOG ( PDERROR, "Session[%s] Failed to create insertor for "
                  "insert: %s", sessionName(), e.what () ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }

   done:
      PD_TRACE_EXITRC ( SDB__CLSSHDSESS__ONINSTREQMSG, rc ) ;
      return rc ;
   error:
      goto done ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__ONDELREQMSG, "_clsShdSession::_onDeleteReqMsg" )
   INT32 _clsShdSession::_onDeleteReqMsg ( NET_HANDLE handle, MsgHeader * msg,
                                           INT64 &delNum )
   {
      PD_LOG ( PDDEBUG, "session[%s] _onDeleteReqMsg", sessionName() ) ;

      INT32 rc = SDB_OK ;
      PD_TRACE_ENTRY ( SDB__CLSSHDSESS__ONDELREQMSG ) ;
      INT32 flags = 0 ;
      CHAR *pCollectionName = NULL ;
      CHAR *pDeletorBuffer = NULL ;
      CHAR *pHintBuffer = NULL ;
      MsgOpDelete * pDelete = (MsgOpDelete*)msg ;
      INT16 w = 0 ;
      INT16 clientW = pDelete->w ;
      INT16 replSize = 0 ;

      rc = _checkWriteStatus() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "failed to check write status:%d", rc ) ;
         goto error ;
      }

      rc = msgExtractDelete ( (CHAR *)msg , &flags, &pCollectionName,
                              &pDeletorBuffer, &pHintBuffer ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG ( PDERROR, "Session[%s] extract delete msg failed[rc:%d]",
            sessionName(), rc ) ;
         goto error ;
      }
      _pCollectionName = pCollectionName ;

      rc = _checkCLStatusAndGetSth( pCollectionName,
                                    pDelete->version,
                                    &_isMainCL, &replSize ) ;
      if ( SDB_OK != rc )
      {
         goto error ;
      }

      rc = _calculateW( &replSize, &clientW, w ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to calculate w:%d", rc ) ;
         goto error ;
      }

      try
      {
         BSONObj deletor ( pDeletorBuffer ) ;
         BSONObj hint ( pHintBuffer ) ;
         // add last op info
         MON_SAVE_OP_DETAIL( eduCB()->getMonAppCB(), msg->opCode,
                             "Collection:%s, Deletor:%s, Hint:%s, "
                             "Flag:0x%08x(%u)",
                             pCollectionName,
                             deletor.toString().c_str(),
                             hint.toString().c_str(),
                             flags, flags ) ;

         PD_LOG ( PDDEBUG, "Session[%s] Delete: deletor: %s\nhint: %s",
                  sessionName(), deletor.toString().c_str(),
                  hint.toString().c_str() ) ;

         if ( _isMainCL )
         {
            rc = _deleteToMainCL( pCollectionName, deletor, hint, flags,
                                  _pEDUCB, _pDmsCB, _pDpsCB, w,
                                  ( pDelete->flags & FLG_DELETE_RETURNNUM ) ?
                                  &delNum : NULL );
         }
         else
         {

            rc = rtnDelete( pCollectionName, deletor, hint, flags, _pEDUCB,
                            _pDmsCB, _pDpsCB, w,
                            ( pDelete->flags & FLG_DELETE_RETURNNUM ) ?
                            &delNum : NULL ) ;
         }
      }
      catch ( std::exception &e )
      {
         PD_LOG ( PDERROR, "Session[%s] Failed to create deletor for "
                  "DELETE: %s", sessionName(), e.what () ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }

   done:
      PD_TRACE_EXITRC ( SDB__CLSSHDSESS__ONDELREQMSG, rc ) ;
      return rc ;
   error:
      goto done ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__ONQYREQMSG, "_clsShdSession::_onQueryReqMsg" )
   INT32 _clsShdSession::_onQueryReqMsg ( NET_HANDLE handle, MsgHeader * msg,
                                          rtnContextBuf &buffObj,
                                          INT32 &startingPos,
                                          INT64 &contextID )
   {
      PD_LOG ( PDDEBUG, "session[%s] _onQueryReqMsg", sessionName() ) ;

      INT32 rc = SDB_OK ;
      PD_TRACE_ENTRY ( SDB__CLSSHDSESS__ONQYREQMSG ) ;
      INT32 flags = 0 ;
      CHAR *pCollectionName = NULL ;
      CHAR *pQueryBuff = NULL ;
      CHAR *pFieldSelector = NULL ;
      CHAR *pOrderByBuffer = NULL ;
      CHAR *pHintBuffer = NULL ;
      INT64 numToSkip = -1 ;
      INT64 numToReturn = -1 ;
      MsgOpQuery *pQuery = (MsgOpQuery*)msg ;
      INT16 clientW = pQuery->w ;
      INT16 replSize = 0 ;
      INT16 w = 1 ;
      _rtnCommand *pCommand = NULL ;

      rc = msgExtractQuery ( (CHAR *)msg, &flags, &pCollectionName,
                             &numToSkip, &numToReturn, &pQueryBuff,
                             &pFieldSelector, &pOrderByBuffer, &pHintBuffer ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG ( PDERROR, "Session[%s] extract query msg failed[rc:%d]",
                  sessionName(), rc ) ;
         goto error ;
      }

      if ( !rtnIsCommand ( pCollectionName ) )
      {
         rtnContextBase *pContext = NULL ;
         _pCollectionName = pCollectionName ;

         if ( flags & FLG_QUERY_MODIFY )
         {
            rc = _checkWriteStatus() ;
            if ( SDB_OK != rc )
            {
               PD_LOG( PDWARNING, "failed to check write status:%d", rc ) ;
               goto error ;
            }

            rc = _checkCLStatusAndGetSth( pCollectionName,
                                          pQuery->version,
                                          &_isMainCL, &replSize ) ;
            if ( SDB_OK != rc )
            {
               goto error ;
            }

            rc = _calculateW( &replSize, &clientW, w ) ;
            if ( SDB_OK != rc )
            {
               PD_LOG( PDERROR, "failed to calculate w:%d", rc ) ;
               goto error ;
            }
         }
         else
         {
            rc = _checkPrimaryWhenRead( FLG_QUERY_PRIMARY, flags ) ;
            if ( rc )
            {
               goto error ;
            }
            rc = _checkCLStatusAndGetSth( pCollectionName,
                                          pQuery->version,
                                          &_isMainCL, NULL ) ;
            if ( SDB_OK != rc )
            {
               goto error ;
            }
         }

         try
         {
            BSONObj matcher ( pQueryBuff ) ;
            BSONObj selector ( pFieldSelector ) ;
            BSONObj orderBy ( pOrderByBuffer ) ;
            BSONObj hint ( pHintBuffer ) ;
            // add last op info
            MON_SAVE_OP_DETAIL( eduCB()->getMonAppCB(), msg->opCode,
                                "Collection:%s, Matcher:%s, Selector:%s, "
                                "OrderBy:%s, Hint:%s, Skip:%llu, Limit:%lld, "
                                "Flag:0x%08x(%u)",
                                pCollectionName,
                                matcher.toString().c_str(),
                                selector.toString().c_str(),
                                orderBy.toString().c_str(),
                                hint.toString().c_str(),
                                numToSkip, numToReturn,
                                flags, flags ) ;

            PD_LOG ( PDDEBUG, "Session[%s] Query: matcher: %s\nselector: "
                     "%s\norderBy: %s\nhint:%s", sessionName(),
                     matcher.toString().c_str(), selector.toString().c_str(),
                     orderBy.toString().c_str(), hint.toString().c_str() ) ;

            if ( !_isMainCL )
            {
               rc = rtnQuery( pCollectionName, selector, matcher, orderBy,
                              hint, flags, _pEDUCB, numToSkip, numToReturn,
                              _pDmsCB, _pRtnCB, contextID, &pContext, TRUE ) ;
            }
            else
            {
               rc = _queryToMainCL( pCollectionName, selector, matcher,
                                    orderBy, hint, flags, _pEDUCB, numToSkip,
                                    numToReturn, contextID, &pContext, w ) ;
            }

            if ( rc )
            {
               goto error ;
            }

            /// set write info
            if ( pContext && pContext->isWrite() )
            {
               pContext->setWriteInfo( _pDpsCB, w ) ;
            }

            // query with return data
            if ( ( flags & FLG_QUERY_WITH_RETURNDATA ) && NULL != pContext )
            {
               rc = pContext->getMore( -1, buffObj, _pEDUCB ) ;
               if ( rc || pContext->eof() )
               {
                  _pRtnCB->contextDelete( contextID, _pEDUCB ) ;
                  contextID = -1 ;
               }
               startingPos = ( INT32 )buffObj.getStartFrom() ;

               if ( SDB_DMS_EOC == rc )
               {
                  rc = SDB_OK ;
               }
               else if ( rc )
               {
                  PD_LOG( PDERROR, "Session[%s] failed to query with return "
                          "data, rc: %d", sessionName(), rc ) ;
                  goto error ;
               }
            }
         }
         catch ( std::exception &e )
         {
            PD_LOG ( PDERROR, "Session[%s] Failed to create matcher and "
                     "selector for QUERY: %s", sessionName(), e.what () ) ;
            rc = SDB_INVALIDARG ;
            goto error ;
         }
      }
      else
      {
         _pCollectionName = NULL ;
         _cmdCollectionName.clear() ;

         rc = rtnParserCommand( pCollectionName, &pCommand ) ;

         if ( SDB_OK != rc )
         {
            PD_LOG ( PDERROR, "Parse command[%s] failed[rc:%d]",
                     pCollectionName, rc ) ;
            goto error ;
         }

         rc = rtnInitCommand( pCommand , flags, numToSkip, numToReturn,
                              pQueryBuff, pFieldSelector, pOrderByBuffer,
                              pHintBuffer ) ;
         if ( SDB_OK != rc )
         {
            goto error ;
         }

         if ( NULL != pCommand->collectionFullName() )
         {
            _cmdCollectionName.assign( pCommand->collectionFullName() ) ;
            _pCollectionName = _cmdCollectionName.c_str() ;
         }

         MON_SAVE_CMD_DETAIL( _pEDUCB->getMonAppCB(), pCommand->type(),
                              "Command:%s, Collection:%s, Match:%s, "
                              "Selector:%s, OrderBy:%s, Hint:%s, Skip:%llu, "
                              "Limit:%lld, Flag:0x%08x(%u)",
                              pCollectionName, _cmdCollectionName.c_str(),
                              BSONObj(pQueryBuff).toString().c_str(),
                              BSONObj(pFieldSelector).toString().c_str(),
                              BSONObj(pOrderByBuffer).toString().c_str(),
                              BSONObj(pHintBuffer).toString().c_str(),
                              numToSkip, numToReturn, flags, flags ) ;

         if ( pCommand->writable () )
         {
            rc = _checkWriteStatus() ;
            if ( SDB_OK != rc )
            {
               PD_LOG( PDWARNING, "failed to check write status:%d", rc ) ;
               goto error ;
            }
         }
         else
         {
            rc = _checkPrimaryWhenRead( FLG_QUERY_PRIMARY, flags ) ;
            if ( rc )
            {
               goto error ;
            }
         }

         //check cata
         if ( pCommand->collectionFullName() )
         {
            rc = _checkCLStatusAndGetSth( pCommand->collectionFullName(),
                                          pQuery->version,
                                          &_isMainCL, &replSize ) ;

            if ( SDB_OK != rc )
            {
               goto error ;
            }

            if ( pCommand->writable() )
            {
               rc = _calculateW( &replSize, &clientW, w ) ;
               if ( SDB_OK != rc )
               {
                  PD_LOG( PDERROR, "failed to calculate w:%d", rc ) ;
                  goto error ;
               }
            }
         }
         else if ( CMD_CREATE_COLLECTIONSPACE == pCommand->type() ||
                   CMD_DROP_COLLECTIONSPACE == pCommand->type() )
         {
            rc = _checkReplStatus() ;
            if ( SDB_OK != rc )
            {
               PD_LOG( PDERROR, "failed to check repl status:%d", rc ) ;
               goto error ;
            }
         }

         PD_LOG ( PDDEBUG, "Command: %s", pCommand->name () ) ;

         /// sometimes we can not get catainfo from command
         /// request. here if w < 1, we set it with 1.
         if ( w < 1 )
         {
            w = 1 ;
         }

         if ( _isMainCL )
         {
            rc = _runOnMainCL( pCollectionName, pCommand, flags, numToSkip,
                               numToReturn, pQueryBuff, pFieldSelector,
                               pOrderByBuffer, pHintBuffer, w,
                               pQuery->version, contextID );
         }
         else
         {
            //run command
            rc = rtnRunCommand( pCommand, getServiceType(),
                                _pEDUCB, _pDmsCB, _pRtnCB,
                                _pDpsCB, w, &contextID ) ;
         }
         if ( SDB_OK != rc )
         {
            PD_LOG( PDERROR, "Run command[%s] failed, rc: %d",
                    pCommand->name(), rc ) ;
            goto error ;
         }

         /// rename collection[space] should to remove catalog
         /// drop collection/space remove catalog in context already
         if ( CMD_RENAME_COLLECTION == pCommand->type() )
         {
            _pCatAgent->lock_w () ;
            _pCatAgent->clear ( pCommand->collectionFullName() ) ;
            _pCatAgent->release_w () ;

            sdbGetClsCB()->invalidateCata( pCommand->collectionFullName() ) ;
         }
      }

   done:
      if ( pCommand )
      {
         rtnReleaseCommand( &pCommand ) ;
      }
      PD_TRACE_EXITRC ( SDB__CLSSHDSESS__ONQYREQMSG, rc ) ;
      return rc ;
   error:
      goto done ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__ONGETMOREREQMSG, "_clsShdSession::_onGetMoreReqMsg" )
   INT32 _clsShdSession::_onGetMoreReqMsg( MsgHeader * msg,
                                           rtnContextBuf &buffObj,
                                           INT32 & startingPos,
                                           INT64 &contextID )
   {
      PD_LOG ( PDDEBUG, "session[%s] _onGetMoreReqMsg", sessionName() ) ;

      INT32 rc = SDB_OK ;
      PD_TRACE_ENTRY ( SDB__CLSSHDSESS__ONGETMOREREQMSG ) ;
      INT32 numToRead = 0 ;

      rc = msgExtractGetMore ( (CHAR*)msg, &numToRead, &contextID ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG ( PDERROR, "Session[%s] extract GETMORE msg failed[rc:%d]",
                  sessionName(), rc ) ;
         goto error ;
      }

      // add last op info
      MON_SAVE_OP_DETAIL( eduCB()->getMonAppCB(), msg->opCode,
                          "ContextID:%lld, NumToRead:%d",
                          contextID, numToRead ) ;

      PD_LOG ( PDDEBUG, "GetMore: contextID:%lld\nnumToRead: %d", contextID,
               numToRead ) ;

      rc = rtnGetMore ( contextID, numToRead, buffObj, _pEDUCB, _pRtnCB ) ;

      startingPos = ( INT32 )buffObj.getStartFrom() ;

   done:
      PD_TRACE_EXITRC ( SDB__CLSSHDSESS__ONGETMOREREQMSG, rc ) ;
      return rc ;
   error:
      goto done ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__ONKILLCTXREQMSG, "_clsShdSession::_onKillContextsReqMsg" )
   INT32 _clsShdSession::_onKillContextsReqMsg ( NET_HANDLE handle,
                                                 MsgHeader * msg )
   {
      PD_LOG ( PDDEBUG, "session[%s] _onKillContextsReqMsg", sessionName() ) ;

      INT32 rc = SDB_OK ;
      PD_TRACE_ENTRY ( SDB__CLSSHDSESS__ONKILLCTXREQMSG ) ;
      INT32 contextNum = 0 ;
      INT64 *pContextIDs = NULL ;

      rc = msgExtractKillContexts ( (CHAR*)msg, &contextNum, &pContextIDs ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG ( PDERROR, "Session[%s] extract KILLCONTEXT msg failed[rc:%d]",
                  sessionName(), rc ) ;
         goto error ;
      }

      // add last op info
      MON_SAVE_OP_DETAIL( eduCB()->getMonAppCB(), msg->opCode,
                          "ContextNum:%d, ContextID:%lld",
                          contextNum, pContextIDs[0] ) ;

      if ( contextNum > 0 )
      {
         PD_LOG ( PDDEBUG, "KillContext: contextNum:%d\ncontextID: %lld",
                  contextNum, pContextIDs[0] ) ;
      }

      rc = rtnKillContexts ( contextNum, pContextIDs, _pEDUCB, _pRtnCB ) ;

   done:
      PD_TRACE_EXITRC ( SDB__CLSSHDSESS__ONKILLCTXREQMSG, rc ) ;
      return rc ;
   error:
      goto done ;
   }

   INT32 _clsShdSession::_onMsgReq ( NET_HANDLE handle, MsgHeader * msg )
   {
      return rtnMsg( (MsgOpMsg*)msg ) ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__ONINRPTMSG, "_clsShdSession::_onInterruptMsg" )
   INT32 _clsShdSession::_onInterruptMsg ( NET_HANDLE handle, MsgHeader * msg )
   {
      PD_TRACE_ENTRY ( SDB__CLSSHDSESS__ONINRPTMSG ) ;
      //delete all contextID
      if ( _pEDUCB )
      {
         INT64 contextID = -1 ;
         while ( -1 != ( contextID = _pEDUCB->contextPeek() ) )
         {
            _pRtnCB->contextDelete ( contextID, NULL ) ;
         }

         INT32 rcTmp = rtnTransRollback( _pEDUCB, _pDpsCB );
         if ( rcTmp )
         {
            PD_LOG ( PDERROR, "Failed to rollback(rc=%d)", rcTmp ) ;
         }
      }

      PD_TRACE_EXIT ( SDB__CLSSHDSESS__ONINRPTMSG ) ;
      return SDB_OK ;
   }
   INT32 _clsShdSession::_onTransBeginMsg ()
   {
      INT32 rc = _checkWriteStatus() ;
      if ( rc )
      {
         return rc;
      }
      return rtnTransBegin( _pEDUCB ) ;
   }

   INT32 _clsShdSession::_onTransCommitMsg ()
   {
      if ( !(_pReplSet->primaryIsMe ()) )
      {
         return SDB_CLS_NOT_PRIMARY;
      }
      // add last op info
      MON_SAVE_OP_DETAIL( eduCB()->getMonAppCB(), MSG_BS_TRANS_COMMIT_REQ,
                          "TransactionID: 0x%016x(%llu)",
                          eduCB()->getTransID(),
                          eduCB()->getTransID() ) ;
      return rtnTransCommit( _pEDUCB, _pDpsCB );
   }

   INT32 _clsShdSession::_onTransRollbackMsg ()
   {
      if ( !(_pReplSet->primaryIsMe ()) )
      {
         return SDB_CLS_NOT_PRIMARY;
      }
      // add last op info
      MON_SAVE_OP_DETAIL( eduCB()->getMonAppCB(), MSG_BS_TRANS_ROLLBACK_REQ,
                          "TransactionID: 0x%016x(%llu)",
                          eduCB()->getTransID(),
                          eduCB()->getTransID() ) ;
      return rtnTransRollback( _pEDUCB, _pDpsCB ) ;
   }

   INT32 _clsShdSession::_onTransCommitPreMsg( MsgHeader *msg )
   {
      if ( _pEDUCB->getTransID() == DPS_INVALID_TRANS_ID )
      {
         return SDB_DPS_TRANS_NO_TRANS ;
      }
      return SDB_OK ;
   }

   INT32 _clsShdSession::_onTransUpdateReqMsg ( NET_HANDLE handle,
                                                MsgHeader *msg,
                                                INT64 &updateNum )
   {
      if ( _pEDUCB->getTransID() == DPS_INVALID_TRANS_ID )
      {
         return SDB_DPS_TRANS_NO_TRANS;
      }
      return _onUpdateReqMsg( handle, msg, updateNum );
   }

   INT32 _clsShdSession::_onTransInsertReqMsg ( NET_HANDLE handle,
                                                MsgHeader *msg,
                                                INT32 &insertedNum,
                                                INT32 &ignoredNum )
   {
      if ( _pEDUCB->getTransID() == DPS_INVALID_TRANS_ID )
      {
         return SDB_DPS_TRANS_NO_TRANS;
      }
      return _onInsertReqMsg( handle, msg, insertedNum, ignoredNum );
   }

   INT32 _clsShdSession::_onTransDeleteReqMsg ( NET_HANDLE handle,
                                                MsgHeader *msg,
                                                INT64 &delNum )
   {
      if ( _pEDUCB->getTransID() == DPS_INVALID_TRANS_ID )
      {
         return SDB_DPS_TRANS_NO_TRANS;
      }
      return _onDeleteReqMsg( handle, msg, delNum );
   }

   void _clsShdSession::_login()
   {
      UINT32 ip = 0, port = 0 ;

      _delayLogin = FALSE ;
      _logout     = FALSE ;

      if ( !_username.empty() )
      {
         _client.authenticate( _username.c_str(), _passwd.c_str() ) ;
      }

      ossUnpack32From64( identifyID(), ip, port ) ;
      /// set detail name
      CHAR szTmpIP[ 50 ] = { 0 } ;
      ossIP2Str( ip, szTmpIP, sizeof(szTmpIP) - 1 ) ;
      ossSnprintf( _detailName, SESSION_NAME_LEN, "%s,R-IP:%s,R-Port:%u",
                   _pmdAsyncSession::sessionName(), szTmpIP,
                   port ) ;
      eduCB()->setName( _detailName ) ;

      /// audit
      CHAR szTmpID[ 20 ] = { 0 } ;
      ossSnprintf( szTmpID, sizeof(szTmpID) - 1, "%llu", eduID() ) ;
      PD_AUDIT_OP( AUDIT_ACCESS, MSG_AUTH_VERIFY_REQ, AUDIT_OBJ_SESSION,
                   szTmpID, SDB_OK, "User[UserName:%s, FromIP:%s, "
                   "FromPort:%u, FromSession:%llu, FromTID:%u] "
                   "login succeed", _client.getUsername(),
                   szTmpIP, port, identifyEDUID(), identifyTID() ) ;
   }

   INT32 _clsShdSession::_onSessionInitReqMsg ( MsgHeader *msg )
   {
      INT32 rc = SDB_OK ;
      MsgComSessionInitReq *pMsgReq = (MsgComSessionInitReq*)msg ;
      MsgRouteID localRouteID = routeAgent()->localID() ;

      /// check wether the route id is matched
      if ( pMsgReq->dstRouteID.value != localRouteID.value )
      {
         rc = SDB_INVALID_ROUTEID;
         PD_LOG ( PDERROR, "Session init failed: route id does not match."
                  "Message info: [%s], Local route id: %s",
                  msg2String( msg ).c_str(),
                  routeID2String( localRouteID ).c_str() ) ;
      }
      else if ( msg->messageLength > sizeof( MsgComSessionInitReq ) )
      {
         /// set user name info
         try
         {
            BSONObj obj( pMsgReq->data ) ;
            BSONElement user = obj.getField( SDB_AUTH_USER ) ;
            BSONElement passwd = obj.getField( SDB_AUTH_PASSWD ) ;
            BSONElement remoteIP = obj.getField( FIELD_NAME_REMOTE_IP ) ;
            BSONElement remotePort = obj.getField( FIELD_NAME_REMOTE_PORT ) ;

            _client.authenticate( user.valuestrsafe(),
                                  passwd.valuestrsafe() ) ;

            if ( String == remoteIP.type() && NumberInt == remotePort.type() )
            {
               _client.setFromInfo( remoteIP.valuestr(),
                                    (UINT16)remotePort.numberInt() ) ;
            }
         }
         catch( std::exception &e )
         {
            PD_LOG( PDERROR, "Occur exception: %s", e.what() ) ;
            /// do not report error
         }
         /// set the remote info into this session
         setIdentifyInfo( pMsgReq->localIP, pMsgReq->localPort,
                          pMsgReq->localTID, pMsgReq->localSessionID ) ;
         /// inner login
         _login() ;
      }
      return rc ;
   }

   INT32 _clsShdSession::_onTransStopEvnt( pmdEDUEvent *event )
   {
      INT32 rcTmp = rtnTransRollback( _pEDUCB, _pDpsCB ) ;
      if ( rcTmp )
      {
         PD_LOG ( PDERROR, "Failed to rollback(rc=%d)", rcTmp ) ;
      }
      return SDB_OK ;
   }

   INT32 _clsShdSession::_insertToMainCL( BSONObj &objs, INT32 objNum,
                                          INT32 flags, INT16 w,
                                          INT32 &insertedNum,
                                          INT32 &ignoredNum )
   {
      INT32 rc = SDB_OK ;
      ossValuePtr pCurPos = 0 ;
      INT32 totalObjsNum = 0 ;

      try
      {
         PD_CHECK( !objs.isEmpty(), SDB_INVALIDARG, error, PDERROR,
                  "Insert record can't be empty" );
         pCurPos = (ossValuePtr)objs.objdata();
         while ( totalObjsNum < objNum )
         {
            BSONObj subObjsInfo( (const CHAR *)pCurPos );
            INT32 subObjsNum = 0;
            UINT32 subObjsSize = 0;
            const CHAR *pSubCLName = NULL;
            BSONElement beSubObjsNum;
            BSONElement beSubObjsSize;
            BSONElement beSubCLName;
            BSONObj insertor;
            beSubObjsNum = subObjsInfo.getField( FIELD_NAME_SUBOBJSNUM );
            PD_CHECK( beSubObjsNum.isNumber(), SDB_INVALIDARG, error, PDERROR,
                      "Failed to get the field(%s)", FIELD_NAME_SUBOBJSNUM );
            subObjsNum = beSubObjsNum.numberInt();

            beSubObjsSize = subObjsInfo.getField( FIELD_NAME_SUBOBJSSIZE );
            PD_CHECK( beSubObjsSize.isNumber(), SDB_INVALIDARG, error, PDERROR,
                      "Failed to get the field(%s)", FIELD_NAME_SUBOBJSSIZE );
            subObjsSize = beSubObjsSize.numberInt();

            beSubCLName = subObjsInfo.getField( FIELD_NAME_SUBCLNAME );
            PD_CHECK( beSubCLName.type() == String, SDB_INVALIDARG, error,
                      PDERROR, "Failed to get the field(%s)",
                      FIELD_NAME_SUBCLNAME );
            pSubCLName = beSubCLName.valuestr();

            pCurPos += ossAlignX( (ossValuePtr)subObjsInfo.objsize(), 4 );
            ++totalObjsNum;
            insertor = BSONObj( (CHAR *)pCurPos ) ;

      retryInsert:
            INT32 subInsertNum = 0 ;
            INT32 subIgnoredNum = 0 ;
            /// insert to sub collection
            rc = rtnInsert ( pSubCLName, insertor, subObjsNum, flags,
                             _pEDUCB, _pDmsCB, _pDpsCB, w,
                             &subInsertNum, &subIgnoredNum ) ;
            insertedNum += subInsertNum ;
            ignoredNum += subIgnoredNum ;            
            if ( rc )
            {
               rc = _processSubCLResult( rc, pSubCLName, _pCollectionName ) ;
               if ( SDB_OK == rc )
               {
                  goto retryInsert ;
               }
            }
            if( rc )
            {
               PD_LOG( PDERROR, "Session[%s]: Failed to insert to "
                       "sub-collection[%s] of main-collection[%s], rc: %d",
                       sessionName(), pSubCLName, _pCollectionName, rc ) ;
               goto error ;
            }

            /// continue next sub collection
            pCurPos += subObjsSize ;
            totalObjsNum += subObjsNum ;
         }
      }
      catch( std::exception &e )
      {
         PD_LOG( PDERROR, "Occur exception: %s", e.what() ) ;
         rc = SDB_INVALIDARG;
         goto error ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _clsShdSession::_includeShardingOrder( const CHAR *pCollectionName,
                                                const BSONObj &orderBy,
                                                BOOLEAN &result )
   {
      INT32 rc = SDB_OK;
      BSONObj shardingKey;
      _clsCatalogSet *pCataSet = NULL;
      BOOLEAN catLocked = FALSE;
      result = FALSE;
      BOOLEAN isRange = FALSE;
      try
      {
         if ( orderBy.isEmpty() )
         {
            goto done;
         }

         _pCatAgent->lock_r () ;
         catLocked = TRUE;
         pCataSet = _pCatAgent->collectionSet( pCollectionName );
         if ( NULL == pCataSet )
         {
            _pCatAgent->release_r () ;
            catLocked = FALSE;

            rc = SDB_CLS_NO_CATALOG_INFO ;
            PD_LOG( PDERROR, "can not find collection:%s", pCollectionName ) ;
            goto error ;
         }
         isRange = pCataSet->isRangeSharding();
         shardingKey = pCataSet->getShardingKey().getOwned() ;
         _pCatAgent->release_r () ;
         catLocked = FALSE;
         if ( !isRange )
         {
            goto done;
         }
         if ( !shardingKey.isEmpty() )
         {
            result = TRUE;
            BSONObjIterator iterOrder( orderBy );
            BSONObjIterator iterSharding( shardingKey );
            while( iterOrder.more() && iterSharding.more() )
            {
               BSONElement beOrder = iterOrder.next();
               BSONElement beSharding = iterSharding.next();
               if ( 0 != beOrder.woCompare( beSharding ) )
               {
                  result = FALSE;
                  break;
               }
            }
         }
      }
      catch( std::exception &e )
      {
         PD_RC_CHECK( SDB_SYS, PDERROR, "occur unexpected error:%s",
                      e.what() );
      }
   done:
      if ( catLocked )
      {
         _pCatAgent->release_r () ;
      }
      return rc;
   error:
      goto done;
   }

   INT32 _clsShdSession::_queryToMainCL( const CHAR *pCollectionName,
                                         const BSONObj &selector,
                                         const BSONObj &matcher,
                                         const BSONObj &orderBy,
                                         const BSONObj &hint,
                                         SINT32 flags,
                                         pmdEDUCB *cb,
                                         SINT64 numToSkip,
                                         SINT64 numToReturn,
                                         SINT64 &contextID,
                                         _rtnContextBase **ppContext,
                                         INT16 w )
   {
      INT32 rc = SDB_OK;
      std::vector< std::string > strSubCLList;
      BSONObj boNewMatcher;
      rtnContextMainCL *pContextMainCL = NULL;
      BOOLEAN includeShardingOrder = FALSE;
      SINT64 tmpContextID = -1 ;
      _rtnQueryOptions options( matcher,
                                selector,
                                orderBy,
                                hint,
                                pCollectionName,
                                ( FLG_QUERY_EXPLAIN & flags ) ?
                                0 : numToSkip,
                                ( FLG_QUERY_EXPLAIN & flags ) ?
                                -1 : numToReturn,
                                flags, FALSE ) ;

      SDB_ASSERT( pCollectionName, "collection name can't be NULL!" ) ;
      SDB_ASSERT( cb, "educb can't be NULL!" );

      rc = _includeShardingOrder( pCollectionName, orderBy,
                                  includeShardingOrder );
      PD_RC_CHECK( rc, PDERROR,
                   "Failed to check order-key(rc=%d)", rc );

      rc = _getSubCLList( matcher, pCollectionName,
                          boNewMatcher, strSubCLList );
      if ( rc != SDB_OK )
      {
         goto error;
      }

      options._query = boNewMatcher ;

      if ( includeShardingOrder )
      {
         rc = _sortSubCLListByBound( pCollectionName, strSubCLList ) ;
         if ( rc )
         {
            /// can't optimize
            includeShardingOrder = FALSE ;
         }
      }

      rc = _pRtnCB->contextNew( RTN_CONTEXT_MAINCL,
                                (rtnContext **)&pContextMainCL,
                                tmpContextID, cb ) ;
      PD_RC_CHECK( rc, PDERROR,
                   "Failed to create new main-collection context(rc=%d)",
                   rc );

      rc = pContextMainCL->open( options,
                                 strSubCLList,
                                 includeShardingOrder,
                                 cb );
      PD_RC_CHECK( rc, PDERROR,
                   "Open main-collection context failed(rc=%d)",
                   rc );

      pContextMainCL->setWriteInfo( _pDpsCB, w ) ;

      if ( FLG_QUERY_EXPLAIN & flags )
      {
         rc = _aggregateMainCLExplaining( pCollectionName, cb,
                                          tmpContextID,
                                          contextID ) ;
         if ( SDB_OK != rc )
         {
            PD_LOG( PDERROR, "failed to aggregate sub cl info:%d", rc ) ;
            goto error ;
         }
      }
      else
      {
         contextID = tmpContextID ;
         tmpContextID = -1 ;

         if ( ppContext )
         {
            *ppContext = pContextMainCL ;
         }
      }
   done:
      return rc;
   error:
      if ( -1 != contextID )
      {
         _pRtnCB->contextDelete( contextID, cb );
         contextID = -1;
      }
      if ( -1 != tmpContextID )
      {
         _pRtnCB->contextDelete( tmpContextID, cb );
         tmpContextID = -1;
      }
      goto done;
   }

   INT32 _clsShdSession::_sortSubCLListByBound( const CHAR *pCollectionName,
                                                std::vector<std::string> &strSubCLList )
   {
      INT32 rc = SDB_OK ;
      std::vector< std::string > strSubCLListTmp ;
      _clsCatalogSet *pCataSet = NULL ;
      std::vector< std::string >::iterator itTmp ;
      std::vector< std::string >::iterator it ;
      BOOLEAN bFind = FALSE ;

      _pCatAgent->lock_r () ;
      pCataSet = _pCatAgent->collectionSet( pCollectionName ) ;
      if ( NULL == pCataSet )
      {
         _pCatAgent->release_r () ;
         rc = SDB_CLS_NO_CATALOG_INFO ;
         PD_LOG( PDERROR, "can not find collection:%s", pCollectionName ) ;
         goto error ;
      }
      pCataSet->getSubCLList( strSubCLListTmp, SUBCL_SORT_BY_BOUND ) ;
      _pCatAgent->release_r () ;

      itTmp = strSubCLListTmp.begin();
      while( itTmp != strSubCLListTmp.end() )
      {
         bFind = FALSE ;
         it = strSubCLList.begin() ;
         while( it != strSubCLList.end() )
         {
            if ( *itTmp == *it )
            {
               strSubCLList.erase( it ) ;
               bFind = TRUE ;
               break ;
            }
            ++it ;
         }

         if ( !bFind )
         {
            itTmp = strSubCLListTmp.erase( itTmp ) ;
         }
         else
         {
            ++itTmp ;
         }
      }

      /// has some sub cl not found
      if ( strSubCLList.size() > 0 )
      {
         rc = SDB_SYS ;
         itTmp = strSubCLListTmp.begin() ;
         while( itTmp != strSubCLListTmp.end() )
         {
            strSubCLList.push_back( *itTmp ) ;
            ++itTmp ;
         }
      }
      else
      {
         strSubCLList = strSubCLListTmp ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _clsShdSession::_getSubCLList( const BSONObj &matcher,
                                        const CHAR *pCollectionName,
                                        BSONObj &boNewMatcher,
                                        vector< string > &strSubCLList )
   {
      INT32 rc = SDB_OK;

      try
      {
         BSONObjBuilder bobNewMatcher;
         BSONObjIterator iter( matcher );
         while( iter.more() )
         {
            BSONElement beTmp = iter.next();
            if ( beTmp.type() == Array &&
                 0 == ossStrcmp(beTmp.fieldName(), CAT_SUBCL_NAME ) )
            {
               BSONObj boSubCLList = beTmp.embeddedObject();
               BSONObjIterator iterSubCL( boSubCLList );
               while( iterSubCL.more() )
               {
                  BSONElement beSubCL = iterSubCL.next();
                  string strSubCLName = beSubCL.str();
                  if ( !strSubCLName.empty() )
                  {
                     strSubCLList.push_back( strSubCLName );
                  }
               }
            }
            else
            {
               bobNewMatcher.append( beTmp );
            }
         }
         boNewMatcher = bobNewMatcher.obj();
      }
      catch( std::exception &e )
      {
         PD_RC_CHECK( SDB_INVALIDARG, PDERROR,
                      "occur unexpected error:%s",
                      e.what() );
      }

      if ( strSubCLList.empty() )
      {
         rc = _getSubCLList( pCollectionName, strSubCLList ) ;
         if ( rc )
         {
            goto error ;
         }
      }

      PD_CHECK( !strSubCLList.empty(), SDB_INVALID_MAIN_CL, error, PDERROR,
                "main-collection has no sub-collection!" ) ;

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _clsShdSession::_getSubCLList( const CHAR *pCollectionName,
                                        vector< string > &subCLList )
   {
      INT32 rc = SDB_OK ;
      const CHAR *pSubCLName = NULL ;
      vector< string > strSubCLListTmp ;
      clsCatalogSet *pCataSet = NULL ;
      vector< string >::iterator iter ;

      _pCatAgent->lock_r () ;
      pCataSet = _pCatAgent->collectionSet( pCollectionName ) ;
      if ( NULL == pCataSet )
      {
         _pCatAgent->release_r () ;
         rc = SDB_CLS_NO_CATALOG_INFO ;
         PD_LOG( PDERROR, "can not find collection:%s", pCollectionName ) ;
         goto error ;
      }
      pCataSet->getSubCLList( strSubCLListTmp ) ;
      _pCatAgent->release_r() ;

      /// check all sub collection is valid
      iter = strSubCLListTmp.begin() ;
      while( iter != strSubCLListTmp.end() )
      {
         pSubCLName = (*iter).c_str() ;

         _pCatAgent->lock_r() ;
         pCataSet = _pCatAgent->collectionSet( pSubCLName ) ;
         if ( NULL == pCataSet )
         {
            _pCatAgent->release_r() ;

            rc = _pShdMgr->syncUpdateCatalog( pSubCLName ) ;
            if ( SDB_OK == rc )
            {
               continue ;
            }
            else
            {
               goto error ;
            }
         }
         /// not on the node, ignore
         else if ( 0 == pCataSet->groupCount() )
         {
            _pCatAgent->release_r() ;

            _pCatAgent->lock_w() ;
            _pCatAgent->clear( pSubCLName ) ;
            _pCatAgent->release_w() ;

            ++iter ;
            continue ;
         }
         _pCatAgent->release_r() ;

         /// push to list
         subCLList.push_back( *iter ) ;
         ++iter ;
      }

      if ( subCLList.empty() )
      {
         /// is empty main collection
         _pCatAgent->lock_w() ;
         _pCatAgent->clear( pCollectionName ) ;
         _pCatAgent->release_w() ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _clsShdSession::_updateToMainCL( const CHAR *pCollectionName,
                                          const BSONObj &selector,
                                          const BSONObj &updator,
                                          const BSONObj &hint,
                                          SINT32 flags,
                                          pmdEDUCB *cb,
                                          SDB_DMSCB *pDmsCB,
                                          SDB_DPSCB *pDpsCB,
                                          INT16 w,
                                          INT64 *pUpdateNum )
   {
      INT32 rc = SDB_OK;
      BSONObj boNewSelector;
      const CHAR *pSubCLName = NULL ;
      vector< string > strSubCLList ;
      vector< string >::iterator iterSubCLSet ;
      INT64 updateNum = 0 ;
      INT64 numTmp = 0 ;

      rc = _getSubCLList( selector, pCollectionName,
                          boNewSelector, strSubCLList ) ;
      if ( rc != SDB_OK )
      {
         goto error;
      }

      /// update sub collections
      iterSubCLSet = strSubCLList.begin() ;
      while( iterSubCLSet != strSubCLList.end() )
      {
         numTmp = 0 ;
         pSubCLName = (*iterSubCLSet).c_str() ;

         rc = rtnUpdate( pSubCLName, boNewSelector, updator,
                         hint, flags, cb, pDmsCB, pDpsCB, w, &numTmp ) ;
         if ( rc )
         {
            rc = _processSubCLResult( rc, pSubCLName, _pCollectionName ) ;
            if ( SDB_OK == rc )
            {
               continue ;
            }
         }

         if ( rc )
         {
            PD_LOG( PDERROR, "Session[%s]: Update on sub-collection[%s] of "
                    "main-collection[%s] failed, rc: %d",
                    sessionName(), pSubCLName, _pCollectionName, rc ) ;
            goto error ;
         }

         /// continue next sub collection
         updateNum += numTmp ;
         ++iterSubCLSet ;
      }

   done:
      if ( pUpdateNum )
      {
         *pUpdateNum = updateNum;
      }
      return rc ;
   error:
      goto done ;
   }

   INT32 _clsShdSession::_deleteToMainCL ( const CHAR *pCollectionName,
                                           const BSONObj &deletor,
                                           const BSONObj &hint, INT32 flags,
                                           pmdEDUCB *cb,
                                           SDB_DMSCB *dmsCB, SDB_DPSCB *dpsCB,
                                           INT16 w, INT64 *pDelNum )
   {
      INT32 rc = SDB_OK ;
      const CHAR *pSubCLName = NULL ;
      BSONObj boNewDeletor ;
      vector< string > strSubCLList ;
      vector< string >::iterator iterSubCLSet ;
      INT64 delNum = 0 ;
      INT64 numTmp = 0 ;

      rc = _getSubCLList( deletor, pCollectionName,
                          boNewDeletor, strSubCLList ) ;
      if ( rc != SDB_OK )
      {
         goto error;
      }

      iterSubCLSet = strSubCLList.begin() ;
      while( iterSubCLSet != strSubCLList.end() )
      {
         numTmp = 0 ;
         pSubCLName = (*iterSubCLSet).c_str() ;

         rc = rtnDelete( pSubCLName, boNewDeletor, hint,
                         flags, cb, dmsCB, dpsCB, w, &numTmp ) ;
         if ( rc )
         {
            rc = _processSubCLResult( rc, pSubCLName, _pCollectionName ) ;
            if ( SDB_OK == rc )
            {
               continue ;
            }
         }

         if ( rc )
         {
            PD_LOG( PDERROR, "Session[%s]: Delete on sub-collection[%s] of "
                    "main-collection[%s] failed, rc: %d", sessionName(),
                    pSubCLName, _pCollectionName, rc ) ;
            goto error ;
         }

         /// continue next sub collection
         delNum += numTmp;
         ++iterSubCLSet;
      }

   done:
      if ( pDelNum )
      {
         *pDelNum = delNum;
      }
      return rc;
   error:
      goto done;
   }

   INT32 _clsShdSession::_runOnMainCL( const CHAR *pCommandName,
                                       _rtnCommand *pCommand,
                                       INT32 flags,
                                       INT64 numToSkip,
                                       INT64 numToReturn,
                                       const CHAR *pQuery,
                                       const CHAR *pField,
                                       const CHAR *pOrderBy,
                                       const CHAR *pHint,
                                       INT16 w,
                                       INT32 version,
                                       SINT64 &contextID )
   {
      INT32 rc = SDB_OK;
      BOOLEAN writable = FALSE ;
      SDB_ASSERT( pCommandName && pCommand, "pCommand can't be null!" );
      switch( pCommand->type() )
      {
      case CMD_GET_COUNT:
      case CMD_GET_INDEXES:
         rc = _getOnMainCL( pCommandName, pCommand->collectionFullName(),
                            flags, numToSkip, numToReturn, pQuery, pField,
                            pOrderBy, pHint, w, contextID );
         break;

      case CMD_CREATE_INDEX:
         writable = TRUE ;
         rc = _createIndexOnMainCL( pCommandName,
                                    pCommand->collectionFullName(),
                                    pQuery, pHint, w, contextID );
         break;

      case CMD_ALTER_COLLECTION :
         writable = TRUE ;
         rc = _alterMainCL( pCommand, _pEDUCB, _pDpsCB ) ;
         break ;
      case CMD_DROP_INDEX:
         writable = TRUE ;
         rc = _dropIndexOnMainCL( pCommandName, pCommand->collectionFullName(),
                                  pQuery, w, contextID );
         break;
      case CMD_TEST_COLLECTION:
         rc = _testMainCollection( pCommand->collectionFullName() ) ;
         break ;
      case CMD_LINK_COLLECTION:
      case CMD_UNLINK_COLLECTION:
         rc = rtnRunCommand( pCommand, CMD_SPACE_SERVICE_SHARD,
                             _pEDUCB, _pDmsCB, _pRtnCB,
                             _pDpsCB, w, &contextID ) ;
         break;

      case CMD_DROP_COLLECTION:
         /// wait sync in context, not set writable
         rc = _dropMainCL( pCommand->collectionFullName(), w,
                           version, contextID );
         break;

      case CMD_TRUNCATE:
         writable = TRUE ;
         rc = _truncateMainCL( pCommand->collectionFullName() ) ;
         break ;
      default:
         rc = SDB_MAIN_CL_OP_ERR;
         break;
      }
      PD_RC_CHECK( rc, PDERROR,
                   "failed to run command on main-collection(rc=%d)",
                   rc );

      /// wait for sync
      if ( writable && w > 1 )
      {
         _pDpsCB->completeOpr( eduCB(), w ) ;
      }

   done:
      return rc;
   error:
      if ( -1 != contextID )
      {
         _pRtnCB->contextDelete( contextID, eduCB() ) ;
         contextID = -1 ;
      }
      goto done;
   }

   INT32 _clsShdSession::_getOnMainCL( const CHAR *pCommand,
                                       const CHAR *pCollection,
                                       INT32 flags,
                                       INT64 numToSkip,
                                       INT64 numToReturn,
                                       const CHAR *pQuery,
                                       const CHAR *pField,
                                       const CHAR *pOrderBy,
                                       const CHAR *pHint,
                                       INT16 w,
                                       SINT64 &contextID )
   {
      INT32 rc = SDB_OK;
      const CHAR *pSubCLName = NULL ;
      vector< string > strSubCLList ;
      vector< string >::iterator iterSubCLSet ;
      BSONObj boNewMatcher;
      rtnContextMainCL *pContextMainCL = NULL;
      BSONObj boMatcher;
      BSONObj boEmpty;
      BSONObj boHint;
      _rtnCommand *pCommandTmp = NULL;
      INT64 subNumToReturn = numToReturn ;
      INT64 subNumToSkip = 0 ;
      SDB_ASSERT( pCommand, "pCommand can't be null!" );
      SDB_ASSERT( pCollection,
                  "collection name can't be null!"  );

      try
      {
         boMatcher = BSONObj( pQuery );
         BSONObj boHintTmp = BSONObj( pHint );
         BSONObjBuilder bobHint;
         BSONObjIterator iter( boHintTmp );
         while( iter.more() )
         {
            BSONElement beTmp = iter.next();
            if ( 0 != ossStrcmp( beTmp.fieldName(), FIELD_NAME_COLLECTION ))
            {
               bobHint.append( beTmp );
            }
         }
         boHint = bobHint.obj();
      }
      catch ( std::exception &e )
      {
         PD_LOG ( PDERROR, "Session[%s] Failed to create matcher: %s",
                  sessionName(), e.what () ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }
      rc = _getSubCLList( boMatcher, pCollection, boNewMatcher,
                          strSubCLList );
      PD_RC_CHECK( rc, PDERROR, "failed to get sub-collection list(rc=%d)",
                   rc );

      /// reset num to skip and num to return
      if ( strSubCLList.size() <= 1 )
      {
         subNumToSkip = numToSkip ;
         numToSkip = 0 ;
      }
      else
      {
         if ( numToSkip > 0 && numToReturn > 0 )
         {
            subNumToReturn = numToSkip + numToReturn ;
         }
      }

      rc = _pRtnCB->contextNew( RTN_CONTEXT_MAINCL,
                                (rtnContext **)&pContextMainCL,
                                contextID, _pEDUCB );
      PD_RC_CHECK( rc, PDERROR,
                  "failed to create new main-collection context(rc=%d)",
                  rc );
      rc = pContextMainCL->open( boEmpty, numToReturn, numToSkip ) ;
      PD_RC_CHECK( rc, PDERROR, "open main-collection context failed(rc=%d)",
                   rc );

      iterSubCLSet = strSubCLList.begin() ;
      while( iterSubCLSet != strSubCLList.end() )
      {
         pSubCLName = (*iterSubCLSet).c_str() ;

         SINT64 subContextID = -1 ;
         BSONObj boSubHint ;
         try
         {
            BSONObjBuilder bobSubHint;
            bobSubHint.appendElements( boHint );
            bobSubHint.append( FIELD_NAME_COLLECTION, *iterSubCLSet ) ;
            boSubHint = bobSubHint.obj();
         }
         catch( std::exception &e )
         {
            PD_LOG ( PDERROR, "Session[%s] Failed to create hint: %s",
                     sessionName(), e.what () ) ;
            rc = SDB_INVALIDARG ;
            goto error ;
         }

         do
         {
            rc = rtnParserCommand( pCommand, &pCommandTmp );
            if ( rc )
            {
               PD_LOG( PDERROR, "Session[%s]: Parse command[%s] failed, "
                       "rc: %d", sessionName(), pCommand, rc ) ;
               break ;
            }
            rc = rtnInitCommand( pCommandTmp, flags, subNumToSkip,
                                 subNumToReturn, boNewMatcher.objdata(),
                                 pField, pOrderBy,
                                 boSubHint.objdata() ) ;
            if ( rc )
            {
               PD_LOG( PDERROR, "Session[%s]: Failed to init command[%s], "
                       "rc: %d", sessionName(), pCommand, rc ) ;
               break ;
            }

            rc = rtnRunCommand( pCommandTmp, CMD_SPACE_SERVICE_SHARD, _pEDUCB,
                                _pDmsCB, _pRtnCB, _pDpsCB, w, &subContextID );
            if ( rc )
            {
               PD_LOG( PDERROR, "Session[%s]: Failed to run command[%s] on "
                       "sub-collection[%s], rc: %d", sessionName(), pCommand,
                       pSubCLName, rc ) ;
               break ;
            }
         } while( FALSE ) ;

         if ( pCommandTmp )
         {
            rtnReleaseCommand( &pCommandTmp ) ;
            pCommandTmp = NULL;
         }

         if ( rc )
         {
            rc = _processSubCLResult( rc, pSubCLName, pCollection ) ;
            if ( SDB_OK == rc )
            {
               continue ;
            }
            goto error ;
         }

         pContextMainCL->addSubContext( subContextID ) ;
         ++iterSubCLSet ;
      }

   done:
      if ( pCommandTmp )
      {
         rtnReleaseCommand( &pCommandTmp );
      }
      return rc;
   error:
      goto done;
   }

   INT32 _clsShdSession::_createIndexOnMainCL( const CHAR *pCommand,
                                               const CHAR *pCollection,
                                               const CHAR *pQuery,
                                               const CHAR *pHint,
                                               INT16 w,
                                               SINT64 &contextID,
                                               BOOLEAN syscall )
   {
      INT32 rc = SDB_OK;
      const CHAR *pSubCLName = NULL ;
      BSONObj boMatcher ;
      BSONObj boNewMatcher ;
      BSONObj boIndex ;
      BSONObj boHint ;
      vector< string > strSubCLList ;
      vector< string >::iterator iter ;
      INT32 sortBufferSize = SDB_INDEX_SORT_BUFFER_DEFAULT_SIZE ;

      try
      {
         boMatcher = BSONObj( pQuery ) ;
         rc = rtnGetObjElement( boMatcher, FIELD_NAME_INDEX, boIndex ) ;
         PD_RC_CHECK( rc, PDERROR, "Failed to get object index, rc: %d",
                      rc ) ;

         if ( NULL != pHint )
         {
            boHint = BSONObj( pHint ) ;

            if ( boHint.hasField( IXM_FIELD_NAME_SORT_BUFFER_SIZE ) )
            {
               rc = rtnGetIntElement( boHint, IXM_FIELD_NAME_SORT_BUFFER_SIZE,
                                      sortBufferSize ) ;
               if ( SDB_OK != rc )
               {
                  PD_LOG ( PDERROR, "Failed to get index sort buffer, hint: %s",
                           boHint.toString().c_str() ) ;
                  goto error ;
               }

               if ( sortBufferSize < 0 )
               {
                  PD_LOG ( PDERROR, "invalid index sort buffer size: %d",
                           sortBufferSize ) ;
                  rc = SDB_INVALIDARG ;
                  goto error ;
               }
            }
         }
      }
      catch( std::exception &e )
      {
         PD_RC_CHECK( SDB_INVALIDARG, PDERROR,
                      "occur unexpected error(%s)",
                      e.what() );
      }

      rc = _getSubCLList( boMatcher, pCollection, boNewMatcher,
                          strSubCLList );
      PD_RC_CHECK( rc, PDERROR, "Failed to get sub-collection list, rc: %d",
                   rc ) ;

      iter = strSubCLList.begin() ;
      while( iter != strSubCLList.end() )
      {
         INT32 rcTmp = SDB_OK ;
         pSubCLName = iter->c_str() ;

         rcTmp = rtnCreateIndexCommand( pSubCLName, boIndex, _pEDUCB,
                                        _pDmsCB, _pDpsCB, syscall,
                                        sortBufferSize ) ;
         if ( rcTmp )
         {
            rcTmp = _processSubCLResult( rcTmp, pSubCLName,
                                         pCollection ) ;
            if ( SDB_OK == rcTmp )
            {
               continue ;
            }
         }

         if ( rcTmp && SDB_OK != rcTmp && SDB_IXM_REDEF != rcTmp )
         {
            PD_LOG( PDERROR, "Session[%s]: Create index[%s] for "
                    "sub-collection[%s] of main-collection[%s] failed, "
                    "rc: %d", sessionName(), boIndex.toString().c_str(),
                    pSubCLName, _pCollectionName, rcTmp ) ;

            if ( SDB_OK == rc )
            {
               rc = rcTmp ;
            }
         }
         ++iter ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _clsShdSession::_dropIndexOnMainCL( const CHAR *pCommand,
                                             const CHAR *pCollection,
                                             const CHAR *pQuery,
                                             INT16 w,
                                             SINT64 &contextID,
                                             BOOLEAN syscall )
   {
      INT32 rc = SDB_OK ;
      const CHAR *pSubCLName = NULL ;
      BSONObj boMatcher ;
      BSONObj boNewMatcher ;
      BSONObj boIndex ;
      vector< string > strSubCLList ;
      vector< string >::iterator iter ;
      BSONElement ele;
      BOOLEAN isExist = FALSE ;

      try
      {
         boMatcher = BSONObj( pQuery );
         rc = rtnGetObjElement( boMatcher, FIELD_NAME_INDEX,
                                boIndex );
         PD_RC_CHECK( rc, PDERROR,
                      "Failed to get object index(rc=%d)", rc );
         ele = boIndex.firstElement() ;
      }
      catch( std::exception &e )
      {
         PD_RC_CHECK( SDB_INVALIDARG, PDERROR,
                      "occur unexpected error(%s)",
                      e.what() );
      }

      rc = _getSubCLList( boMatcher, pCollection, boNewMatcher,
                          strSubCLList );
      PD_RC_CHECK( rc, PDERROR,
                   "Failed to get sub-collection list, rc: %d", rc ) ;

      iter = strSubCLList.begin();
      while( iter != strSubCLList.end() )
      {
         INT32 rcTmp = SDB_OK ;
         pSubCLName = iter->c_str() ;

         rcTmp = rtnDropIndexCommand( pSubCLName, ele, _pEDUCB,
                                      _pDmsCB, _pDpsCB, syscall ) ;
         if ( rcTmp )
         {
            rcTmp = _processSubCLResult( rcTmp, pSubCLName, pCollection ) ;
            if ( SDB_OK == rcTmp )
            {
               continue ;
            }
         }

         if ( SDB_OK == rcTmp )
         {
            isExist = TRUE ;
         }
         else
         {
            if ( SDB_OK == rc || SDB_IXM_NOTEXIST == rc )
            {
               rc = rcTmp ;
            }
            PD_LOG( PDERROR, "Session[%s]: Drop index[%s] for "
                    "sub-collection[%s] of main-collection[%s] "
                    "failed, rc: %d", sessionName(), ele.toString().c_str(),
                    pSubCLName, _pCollectionName, rcTmp ) ;
         }
         ++iter ;
      }

      if ( SDB_IXM_NOTEXIST == rc && isExist )
      {
         rc = SDB_OK ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _clsShdSession::_dropMainCL( const CHAR *pCollection,
                                      INT16 w,
                                      INT32 version,
                                      SINT64 &contextID )
   {
      INT32 rc = SDB_OK ;
      vector< string > subCLLst ;
      contextID = -1 ;
      rtnContextDelMainCL *delContext = NULL ;

      rc = _getSubCLList( pCollection, subCLLst ) ;
      PD_RC_CHECK( rc, PDERROR, "Session[%s]: Failed to get sub collection "
                   "list, rc: %d", sessionName(), rc ) ;

      rc = _pRtnCB->contextNew( RTN_CONTEXT_DELMAINCL,
                                (rtnContext **)&delContext,
                                contextID, _pEDUCB );
      PD_RC_CHECK( rc, PDERROR, "Failed to create context, drop "
                   "main collection[%s] failed, rc: %d", pCollection,
                   rc ) ;
      rc = delContext->open( pCollection, subCLLst, version, _pEDUCB, w ) ;
      PD_RC_CHECK( rc, PDERROR, "Failed to open context, drop "
                   "main collection[%s] failed, rc: %d", pCollection,
                   rc ) ;

   done:
      return rc;
   error:
      goto done;
   }

   INT32 _clsShdSession::_onCatalogChangeNtyMsg( MsgHeader * msg )
   {
      _pShdMgr->updateCatGroup() ;
      return SDB_OK ;
   }

   INT32 _clsShdSession::_aggregateMainCLExplaining( const CHAR *fullName,
                                                     pmdEDUCB *cb,
                                                     SINT64 &mainCLContextID,
                                                     SINT64 &contextID )
   {
      INT32 rc = SDB_OK ;
      BSONObjBuilder builder ;
      BSONArrayBuilder arrBuilder ;
      BSONObj obj ;
      SDB_RTNCB *rtnCB = sdbGetRTNCB() ;
      _rtnContextDump *context = NULL ;
      BOOLEAN extractNode = FALSE ;

      rc = rtnCB->contextNew ( RTN_CONTEXT_DUMP,
                               (rtnContext**)&context,
                               contextID, cb ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to create new context:%d", rc ) ;
         goto error ;
      }

      rc = context->open( BSONObj(), BSONObj() ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to open context:%d", rc ) ;
         goto error ;
      }

      builder.append( FIELD_NAME_NAME, fullName ) ;
      {
      rtnDataSet dataSet( mainCLContextID, cb ) ;
      while ( TRUE )
      {
         BSONObjBuilder tmp ;
         BSONElement ele ;
         rc = dataSet.next( obj ) ;
         if ( SDB_OK != rc )
         {
            break ;
         }

         if ( !extractNode )
         {
            ele = obj.getField( FIELD_NAME_NODE_NAME ) ;
            if ( String != ele.type() )
            {
               PD_LOG( PDERROR, "invalid result of explaining:%s",
                       obj.toString( FALSE, TRUE ).c_str() ) ;
               rc = SDB_SYS ;
               goto error ;
            }
            builder.append( ele ) ;

            ele = obj.getField( FIELD_NAME_GROUPNAME ) ;
            if ( String == ele.type() )
            {
               builder.append( ele ) ;
            }
            extractNode = TRUE ;
         }

         ele = obj.getField( FIELD_NAME_NAME ) ;
         if ( String != ele.type() )
         {
            PD_LOG( PDERROR, "invalid result of explaining:%s",
                    obj.toString( FALSE, TRUE ).c_str() ) ;
            rc = SDB_SYS ;
            goto error ;
         }
         tmp.append( ele ) ;

         ele = obj.getField( FIELD_NAME_USE_EXT_SORT ) ;
         if ( Bool != ele.type() )
         {
            PD_LOG( PDERROR, "invalid result of explaining:%s",
                    obj.toString( FALSE, TRUE ).c_str() ) ;
            rc = SDB_SYS ;
            goto error ;
         }
         tmp.append( ele ) ;

         ele = obj.getField( FIELD_NAME_SCANTYPE ) ;
         if ( String != ele.type() )
         {
            PD_LOG( PDERROR, "invalid result of explaining:%s",
                    obj.toString( FALSE, TRUE ).c_str() ) ;
            rc = SDB_SYS ;
            goto error ;
         }
         tmp.append( ele ) ;

         ele = obj.getField( FIELD_NAME_INDEXNAME ) ;
         if ( String != ele.type() )
         {
            PD_LOG( PDERROR, "invalid result of explaining:%s",
                    obj.toString( FALSE, TRUE ).c_str() ) ;
            rc = SDB_SYS ;
            goto error ;
         }
         tmp.append( ele ) ;

         ele = obj.getField( FIELD_NAME_RETURN_NUM ) ;
         if ( !ele.isNumber() )
         {
            PD_LOG( PDERROR, "invalid result of explaining:%s",
                    obj.toString( FALSE, TRUE ).c_str() ) ;
            rc = SDB_SYS ;
            goto error ;
         }
         tmp.append( ele ) ;

         ele = obj.getField( FIELD_NAME_ELAPSED_TIME ) ;
         if ( !ele.isNumber() )
         {
            PD_LOG( PDERROR, "invalid result of explaining:%s",
                    obj.toString( FALSE, TRUE ).c_str() ) ;
            rc = SDB_SYS ;
            goto error ;
         }
         tmp.append( ele ) ;

         ele = obj.getField( FIELD_NAME_INDEXREAD ) ;
         if ( !ele.isNumber() )
         {
            PD_LOG( PDERROR, "invalid result of explaining:%s",
                    obj.toString( FALSE, TRUE ).c_str() ) ;
            rc = SDB_SYS ;
            goto error ;
         }
         tmp.append( ele ) ;

         ele = obj.getField( FIELD_NAME_DATAREAD ) ;
         if ( !ele.isNumber() )
         {
            PD_LOG( PDERROR, "invalid result of explaining:%s",
                    obj.toString( FALSE, TRUE ).c_str() ) ;
            rc = SDB_SYS ;
            goto error ;
         }
         tmp.append( ele ) ;

         ele = obj.getField( FIELD_NAME_USERCPU ) ;
         if ( !ele.isNumber() )
         {
            PD_LOG( PDERROR, "invalid result of explaining:%s",
                    obj.toString( FALSE, TRUE ).c_str() ) ;
            rc = SDB_SYS ;
            goto error ;
         }
         tmp.append( ele ) ;

         ele = obj.getField( FIELD_NAME_SYSCPU ) ;
         if ( !ele.isNumber() )
         {
            PD_LOG( PDERROR, "invalid result of explaining:%s",
                    obj.toString( FALSE, TRUE ).c_str() ) ;
            rc = SDB_SYS ;
            goto error ;
         }
         tmp.append( ele ) ;

         ele = obj.getField( FIELD_NAME_QUERY ) ;
         if ( !ele.eoo() )
         {
            tmp.append( ele ) ;
         }

         ele = obj.getField( FIELD_NAME_IX_BOUND ) ;
         if ( !ele.eoo() )
         {
            tmp.append( ele ) ;
         }

         ele = obj.getField( FIELD_NAME_NEED_MATCH ) ;
         if ( !ele.eoo() )
         {
            tmp.append( ele ) ;
         }

         arrBuilder << tmp.obj() ;
      }

      if ( SDB_DMS_EOC != rc )
      {
         PD_LOG( PDERROR, "failed to get the next obj:%d", rc ) ;
         goto error ;
      }
      mainCLContextID = -1 ;

      builder.append( FIELD_NAME_SUB_COLLECTIONS, arrBuilder.arr() ) ;
      }

      rc = context->monAppend( builder.obj() ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to append obj to context:%d", rc ) ;
         goto error ;
      }


   done:
      return rc ;
   error:
      if ( -1 != contextID )
      {
         rtnCB->contextDelete( contextID, cb ) ;
         contextID = -1 ;
      }
      goto done ;
   }

   INT32 _clsShdSession::_onOpenLobReq( MsgHeader *msg,
                                        SINT64 &contextID,
                                        rtnContextBuf &buffObj )
   {
      INT32 rc = SDB_OK ;
      const MsgOpLob *header = NULL ;
      BSONObj lob ;
      BSONObj meta ;
      BSONElement fullName ;
      BSONElement mode ;
      INT16 w = 0 ;
      INT16 replSize = 0 ;
      _rtnContextShdOfLob *context = NULL ;
      SDB_RTNCB *rtnCB = sdbGetRTNCB() ;

      rc = msgExtractOpenLobRequest( ( const CHAR * )msg, &header, lob ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to extract open msg:%d", rc ) ;
         goto error ;
      }
      fullName = lob.getField( FIELD_NAME_COLLECTION ) ;
      if ( String != fullName.type() )
      {
         PD_LOG( PDERROR, "invalid lob obj:%s",
                 lob.toString( FALSE, TRUE ).c_str() ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }
      _pCollectionName = fullName.valuestr() ;

      mode = lob.getField( FIELD_NAME_LOB_OPEN_MODE ) ;
      if ( NumberInt != mode.type() )
      {
         PD_LOG( PDERROR, "invalid lob obj:%s",
                 lob.toString( FALSE, TRUE ).c_str() ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }

      // add last op info
      MON_SAVE_OP_DETAIL( eduCB()->getMonAppCB(), msg->opCode,
                          "Option:%s", lob.toString().c_str() ) ;

      if ( SDB_LOB_MODE_R != mode.Int() )
      {
         rc = _checkWriteStatus() ;
         if ( SDB_OK != rc )
         {
            PD_LOG( PDWARNING, "failed to check write status:%d", rc ) ;
            goto error ;
         }
      }
      else
      {
         rc = _checkPrimaryWhenRead( FLG_LOBREAD_PRIMARY, header->flags ) ;
         if ( rc )
         {
            PD_LOG( PDWARNING, "failed to check read status:%d", rc ) ;
            goto error ;
         }
      }

      rc = _checkCLStatusAndGetSth( fullName.valuestr(),
                                    header->version,
                                    &_isMainCL,
                                    &replSize ) ;

      if ( SDB_OK != rc )
      {
         goto error ;
      }

      rc = _calculateW( &replSize, &( header->w ), w ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to calculate w:%d", rc ) ;
         goto error ;
      }

      rc = rtnCB->contextNew( RTN_CONTEXT_SHARD_OF_LOB,
                              (rtnContext**)(&context),
                              contextID, _pEDUCB ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to open context:%d", rc ) ;
         goto error ;
      }

      rc = context->open( lob, header->version, w,
                          _pDpsCB, _pEDUCB, meta ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to open lob context:%d", rc ) ;
         goto error ;
      }

      /// if sequence 0 is not on this node, we have nothing to send back.
      if ( !meta.isEmpty() )
      {
         buffObj = rtnContextBuf( meta.objdata(),
                                  meta.objsize(),
                                  1 ) ;
      }
   done:
      return rc ;
   error:
      if ( -1 != contextID )
      {
         rtnCB->contextDelete( contextID, _pEDUCB ) ;
         contextID = -1 ;
      }
      goto done ;
   }

   INT32 _clsShdSession::_onWriteLobReq( MsgHeader *msg )
   {
      INT32 rc = SDB_OK ;
      const MsgOpLob *header = NULL ;
      BSONObj obj ;
      const MsgLobTuple *tuple = NULL ;
      UINT32 tSize = 0 ;
      const MsgLobTuple *curTuple = NULL ;
      UINT32 tupleNum = 0 ;
      const CHAR *data = NULL ;
      rtnContext *context = NULL ;
      rtnContextShdOfLob *lobContext = NULL ;
      SDB_RTNCB *rtnCB = sdbGetRTNCB() ;
      INT16 w = 0 ;
      INT16 wWhenOpen = 0 ;

      rc = msgExtractLobRequest( ( const CHAR * )msg,
                                 &header, obj,
                                 &tuple, &tSize ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to extract write msg:%d", rc ) ;
         goto error ;
      }

      context = (rtnCB->contextFind ( header->contextID )) ;
      if ( NULL == context )
      {
         PD_LOG ( PDERROR, "context %lld does not exist", header->contextID ) ;
         rc = SDB_RTN_CONTEXT_NOTEXIST ;
         goto error ;
      }

      if ( RTN_CONTEXT_SHARD_OF_LOB != context->getType() )
      {
         PD_LOG( PDERROR, "invalid type of context:%d", context->getType() ) ;
         rc = SDB_SYS ;
         goto error ;
      }

      lobContext = ( rtnContextShdOfLob * )context ;
      _pCollectionName = lobContext->getFullName() ;
      wWhenOpen = lobContext->getW() ;

      // add last op info
      MON_SAVE_OP_DETAIL( eduCB()->getMonAppCB(), msg->opCode,
                          "ContextID:%lld, CollectionName:%s, TupleSize:%u",
                          header->contextID, _pCollectionName, tSize ) ;

      rc = _checkWriteStatus() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "failed to check write status:%d", rc ) ;
         goto error ;
      }

      rc = _checkCLStatusAndGetSth( lobContext->getFullName(),
                                    header->version,
                                    &_isMainCL, NULL ) ;
      if ( SDB_OK != rc )
      {
         goto error ;
      }

      rc = _calculateW( &wWhenOpen, NULL, w ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to calculate w:%d", rc ) ;
         goto error ;
      }

      while ( TRUE )
      {
         BOOLEAN got = FALSE ;
         rc = msgExtractTuplesAndData( &tuple, &tSize,
                                       &curTuple, &data,
                                       &got ) ;
         if ( SDB_OK != rc )
         {
            PD_LOG( PDERROR, "failed to extract next tuple:%d", rc ) ;
            goto error ;
         }

         if ( !got )
         {
            break ;
         }

         rc = lobContext->write( curTuple->columns.sequence,
                                 curTuple->columns.offset,
                                 curTuple->columns.len,
                                 data, _pEDUCB ) ;
         if ( SDB_OK != rc )
         {
            PD_LOG( PDERROR, "failed to write lob:%d", rc ) ;
            goto error ;
         }

         ++tupleNum ;
         if ( 0 == tupleNum % SHD_INTERRUPT_CHECKPOINT &&
              _pEDUCB->isInterrupted() )
         {
            rc = SDB_APP_INTERRUPT ;
            goto error ;
         }
      }

      PD_LOG( PDDEBUG, "%d pieces of lob[%s] write done",
              tupleNum, lobContext->getOID().str().c_str() ) ;
   done:
      return rc ;
   error:
      if ( NULL != context &&
           SDB_CLS_COORD_NODE_CAT_VER_OLD != rc &&
           SDB_CLS_DATA_NODE_CAT_VER_OLD != rc )
      {
         rtnCB->contextDelete( context->contextID(), _pEDUCB ) ;
      }
      goto done ;
   }

   INT32 _clsShdSession::_onCloseLobReq( MsgHeader *msg )
   {
      INT32 rc = SDB_OK ;
      const MsgOpLob *header = NULL ;
      rtnContextShdOfLob *lobContext = NULL ;
      rtnContext *context = NULL ;
      SDB_RTNCB *rtnCB = sdbGetRTNCB() ;

      rc = msgExtractCloseLobRequest( ( const CHAR * )msg, &header ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to extract close msg:%d", rc ) ;
         goto error ;
      }

      context = rtnCB->contextFind ( header->contextID ) ;
      if ( NULL == context )
      {
         PD_LOG ( PDERROR, "context %lld does not exist",
                  header->contextID ) ;
         /// lob has already been closed.
         goto done ;
      }

      if ( RTN_CONTEXT_SHARD_OF_LOB != context->getType() )
      {
         PD_LOG( PDERROR, "invalid context type:%d", context->getType() ) ;
         rc = SDB_SYS ;
         goto error ;
      }

      /// do not check version coz we will not
      ///  change any thing except close the context.
      lobContext = ( rtnContextShdOfLob * )context ;

      // add last op info
      MON_SAVE_OP_DETAIL( eduCB()->getMonAppCB(), msg->opCode,
                          "ContextID:%lld, Collection:%s",
                          header->contextID,
                          lobContext->getFullName() ) ;

      rc = lobContext->close( _pEDUCB ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to close lob:%d", rc ) ;
         goto error ;
      }

   done:
      if ( NULL != context )
      {
         rtnCB->contextDelete ( context->contextID(), _pEDUCB ) ;
      }
      return rc ;
   error:
      goto done ;
   }

   INT32 _clsShdSession::_onReadLobReq( MsgHeader *msg,
                                        rtnContextBuf &buffObj )
   {
      INT32 rc = SDB_OK ;
      const MsgOpLob *header = NULL ;
      rtnContextShdOfLob *lobContext = NULL ;
      rtnContext *context = NULL ;
      SDB_RTNCB *rtnCB = sdbGetRTNCB() ;
      const MsgLobTuple *tuple = NULL ;
      UINT32 tuplesSize = 0 ;
      bson::BSONObj meta ;
      const CHAR *data = NULL ;
      UINT32 read = 0 ;

      rc = msgExtractLobRequest( ( const CHAR * )msg,
                                 &header, meta, &tuple, &tuplesSize ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to extract read msg:%d", rc ) ;
         goto error ;
      }

      context = rtnCB->contextFind ( header->contextID ) ;
      if ( NULL == context )
      {
         PD_LOG ( PDERROR, "context %lld does not exist",
                  header->contextID ) ;
         rc = SDB_RTN_CONTEXT_NOTEXIST ;
         goto error ;
      }

      if ( RTN_CONTEXT_SHARD_OF_LOB != context->getType() )
      {
         PD_LOG( PDERROR, "invalid context type:%d", context->getType() ) ;
         rc = SDB_SYS ;
         goto error ;
      }

      lobContext = ( rtnContextShdOfLob * )context ;
      _pCollectionName = lobContext->getFullName() ;

      // add last op info
      MON_SAVE_OP_DETAIL( eduCB()->getMonAppCB(), msg->opCode,
                          "ContextID:%lld, Collection:%s, TupleSize:%u",
                          header->contextID, _pCollectionName, tuplesSize ) ;

      rc = _checkPrimaryWhenRead(FLG_LOBREAD_PRIMARY,  header->flags ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "failed to check read status:%d", rc ) ;
         goto error ;
      }

      /// check catalog version
      rc = _checkCLStatusAndGetSth( lobContext->getFullName(),
                                    header->version,
                                    &_isMainCL, NULL ) ;
      if ( SDB_OK != rc )
      {
         goto error ;
      }

      rc = lobContext->readv( tuple, tuplesSize / sizeof( MsgLobTuple ),
                              _pEDUCB, &data, read ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to read lob:%d", rc ) ;
         goto error ;
      }

      buffObj = rtnContextBuf( data, read, 0 ) ;
   done:
      return rc ;
   error:
      if ( NULL != context &&
           SDB_CLS_COORD_NODE_CAT_VER_OLD != rc &&
           SDB_CLS_DATA_NODE_CAT_VER_OLD != rc  )
      {
         rtnCB->contextDelete ( context->contextID(), _pEDUCB ) ;
      }
      goto done ;
   }

   INT32 _clsShdSession::_onRemoveLobReq( MsgHeader *msg )
   {
      INT32 rc = SDB_OK ;
      const MsgOpLob *header = NULL ;
      rtnContextShdOfLob *lobContext = NULL ;
      rtnContext *context = NULL ;
      SDB_RTNCB *rtnCB = sdbGetRTNCB() ;
      const MsgLobTuple *begin = NULL ;
      UINT32 tuplesSize = 0 ;
      BSONObj obj ;
      INT16 w = 0 ;
      INT16 wWhenOpen = 0 ;
      UINT32 tupleNum = 0 ;

      rc = msgExtractLobRequest( ( const CHAR * )msg, &header,
                                 obj, &begin, &tuplesSize ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to extract close msg:%d", rc ) ;
         goto error ;
      }

      context = rtnCB->contextFind ( header->contextID ) ;
      if ( NULL == context )
      {
         PD_LOG ( PDERROR, "context %lld does not exist",
                  header->contextID ) ;
         rc = SDB_RTN_CONTEXT_NOTEXIST ;
         goto error ;
      }

      if ( RTN_CONTEXT_SHARD_OF_LOB != context->getType() )
      {
         PD_LOG( PDERROR, "invalid context type:%d", context->getType() ) ;
         rc = SDB_SYS ;
         goto error ;
      }

      lobContext = ( rtnContextShdOfLob * )context ;
      _pCollectionName = lobContext->getFullName() ;
      wWhenOpen = lobContext->getW() ;

      // add last op info
      MON_SAVE_OP_DETAIL( eduCB()->getMonAppCB(), msg->opCode,
                          "ContextID:%lld, Collection:%s, TupleSize:%u",
                          header->contextID, _pCollectionName,
                          tuplesSize ) ;

      rc = _checkWriteStatus() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "failed to check write status:%d", rc ) ;
         goto error ;
      }

      rc = _checkCLStatusAndGetSth( lobContext->getFullName(),
                                    header->version,
                                    &_isMainCL, NULL ) ;
      if ( SDB_OK != rc )
      {
         goto error ;
      }

      rc = _calculateW( &wWhenOpen, NULL, w ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to calculate w:%d", rc ) ;
         goto error ;
      }

      while ( TRUE )
      {
         BOOLEAN got = FALSE ;
         const MsgLobTuple *curTuple = NULL ;
         rc = msgExtractTuples( &begin, &tuplesSize,
                                &curTuple, &got ) ;
         if ( SDB_OK != rc )
         {
            PD_LOG( PDERROR, "failed to extract next tuple:%d", rc ) ;
            goto error ;
         }

         if ( !got )
         {
            break ;
         }

         rc = lobContext->remove( curTuple->columns.sequence,
                                  _pEDUCB ) ;
         if ( SDB_OK != rc )
         {
            PD_LOG( PDERROR, "failed to remove lob:%d", rc ) ;
            goto error ;
         }

         if ( 0 == ++tupleNum % SHD_INTERRUPT_CHECKPOINT &&
              _pEDUCB->isInterrupted() )
         {
            rc = SDB_APP_INTERRUPT ;
            goto error ;
         }
      }

      PD_LOG( PDDEBUG, "%d pieces of lob[%s] remove done",
              tupleNum, lobContext->getOID().str().c_str() ) ;
   done:
      return rc ;
   error:
      if ( NULL != context &&
           SDB_CLS_COORD_NODE_CAT_VER_OLD != rc &&
           SDB_CLS_DATA_NODE_CAT_VER_OLD != rc  )
      {
         rtnCB->contextDelete ( context->contextID(), _pEDUCB ) ;
      }
      goto done ;
   }

   INT32 _clsShdSession::_onUpdateLobReq( MsgHeader *msg )
   {
      INT32 rc = SDB_OK ;
      const MsgOpLob *header = NULL ;
      BSONObj obj ;
      const MsgLobTuple *tuple = NULL ;
      UINT32 tSize = 0 ;
      const MsgLobTuple *curTuple = NULL ;
      UINT32 tupleNum = 0 ;
      const CHAR *data = NULL ;
      rtnContext *context = NULL ;
      rtnContextShdOfLob *lobContext = NULL ;
      SDB_RTNCB *rtnCB = sdbGetRTNCB() ;
      INT16 w = 0 ;
      INT16 wWhenOpen = 0 ;

      rc = msgExtractLobRequest( ( const CHAR * )msg,
                                 &header, obj,
                                 &tuple, &tSize ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to extract write msg:%d", rc ) ;
         goto error ;
      }

      context = (rtnCB->contextFind ( header->contextID )) ;
      if ( NULL == context )
      {
         PD_LOG ( PDERROR, "context %lld does not exist", header->contextID ) ;
         rc = SDB_RTN_CONTEXT_NOTEXIST ;
         goto error ;
      }

      if ( RTN_CONTEXT_SHARD_OF_LOB != context->getType() )
      {
         PD_LOG( PDERROR, "invalid type of context:%d", context->getType() ) ;
         rc = SDB_SYS ;
         goto error ;
      }

      lobContext = ( rtnContextShdOfLob * )context ;
      _pCollectionName = lobContext->getFullName() ;
      wWhenOpen = lobContext->getW() ;

      // add last op info
      MON_SAVE_OP_DETAIL( eduCB()->getMonAppCB(), msg->opCode,
                          "ContextID:%lld, Collection:%s, TupleSize:%u",
                          header->contextID, _pCollectionName, tSize ) ;

      rc = _checkWriteStatus() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDWARNING, "failed to check write status:%d", rc ) ;
         goto error ;
      }

      rc = _checkCLStatusAndGetSth( lobContext->getFullName(),
                                    header->version,
                                    &_isMainCL, NULL ) ;
      if ( SDB_OK != rc )
      {
         goto error ;
      }

      rc = _calculateW( &wWhenOpen, NULL, w ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to calculate w:%d", rc ) ;
         goto error ;
      }

      while ( TRUE )
      {
         BOOLEAN got = FALSE ;
         rc = msgExtractTuplesAndData( &tuple, &tSize,
                                       &curTuple, &data,
                                       &got ) ;
         if ( SDB_OK != rc )
         {
            PD_LOG( PDERROR, "failed to extract next tuple:%d", rc ) ;
            goto error ;
         }

         if ( !got )
         {
            break ;
         }

         rc = lobContext->update( curTuple->columns.sequence,
                                  curTuple->columns.offset,
                                  curTuple->columns.len,
                                  data, _pEDUCB ) ;
         if ( SDB_OK != rc )
         {
            PD_LOG( PDERROR, "failed to update lob:%d", rc ) ;
            goto error ;
         }

         if ( 0 == ++tupleNum % SHD_INTERRUPT_CHECKPOINT &&
              _pEDUCB->isInterrupted() )
         {
            rc = SDB_APP_INTERRUPT ;
            goto error ;
         }
      }

      PD_LOG( PDDEBUG, "%d pieces of lob[%s] update done",
              tupleNum, lobContext->getOID().str().c_str() ) ;
   done:
      return rc ;
   error:
      if ( NULL != context &&
           SDB_CLS_COORD_NODE_CAT_VER_OLD != rc &&
           SDB_CLS_DATA_NODE_CAT_VER_OLD != rc  )
      {
         rtnCB->contextDelete( context->contextID(), _pEDUCB ) ;
      }
      goto done ;
   }

   INT32 _clsShdSession::_truncateMainCL( const CHAR *fullName )
   {
      INT32 rc = SDB_OK ;
      const CHAR *pSubCLName = NULL ;
      vector< string > subCLs ;
      vector< string >::iterator itr ;

      rc = _getSubCLList( fullName, subCLs ) ;
      PD_RC_CHECK( rc, PDERROR, "Session[%s]: Get sub collection list "
                   "failed, rc: %d", sessionName(), rc ) ;

      itr = subCLs.begin() ;
      while ( itr != subCLs.end() )
      {
         pSubCLName = itr->c_str() ;

         rc = rtnTruncCollectionCommand( pSubCLName, _pEDUCB,
                                         _pDmsCB, _pDpsCB ) ;
         if ( rc )
         {
            rc = _processSubCLResult( rc, pSubCLName, fullName ) ;
            if ( SDB_OK == rc )
            {
               continue ;
            }
         }

         if ( SDB_OK != rc )
         {
            PD_LOG( PDERROR, "Session[%s]: Failed to truncate sub-"
                    "collection[%s] fo main-collection[%s] failed, rc: %d",
                    sessionName(), pSubCLName, fullName, rc ) ;
            goto error ;
         }
         ++itr ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _clsShdSession::_testMainCollection( const CHAR *fullName )
   {
      INT32 rc = SDB_OK ;
      const CHAR *pSubCLName = NULL ;
      vector< string > subCLs ;
      SDB_DMSCB *dmsCB = sdbGetDMSCB() ;
      rc = _getSubCLList( fullName, subCLs ) ;
      PD_RC_CHECK( rc, PDERROR, "Session[%s]: Get sub collection list "
                   "failed, rc: %d", sessionName(), rc ) ;
      for ( vector< string >::iterator itr =  subCLs.begin();
            itr != subCLs.end();
            ++itr )
      {
         pSubCLName = itr->c_str() ;
         rc = rtnTestCollectionCommand( pSubCLName, dmsCB ) ;
         if ( SDB_OK != rc )
         {
            PD_LOG( PDERROR, "failed to test sub collection:%s, rc:%d",
                    pSubCLName, rc ) ;
            goto error ;
         }

      }
   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _clsShdSession::_alterMainCL( _rtnCommand *command,
                                       pmdEDUCB *cb,
                                       SDB_DPSCB *dpsCB )
   {
      INT32 rc = SDB_OK ;
      vector< string > subCLs ;
      const _rtnAlterCollection *alterCommand =
                 ( const _rtnAlterCollection * )command ;
      const _rtnAlterJob &job = alterCommand->getRunner().getJob() ;
      /// do nothing when it is old version
      const BSONObj &tasks = job.isEmpty() ? BSONObj() : job.getTasks() ;
      BSONObjIterator i( tasks ) ;
      while ( i.more() )
      {
         RTN_ALTER_FUNC_TYPE taskType = RTN_ALTER_FUNC_INVALID ;
         BSONObj task ;
         BSONElement e = i.next() ;
         if ( Object != e.type() )
         {
            PD_LOG( PDERROR, "invalid task element" ) ;
            rc = SDB_SYS ;
            goto error ;
         }

         task = e.embeddedObject() ;
         taskType = ( RTN_ALTER_FUNC_TYPE )
                    ( task.getIntField( FIELD_NAME_TASKTYPE ) ) ;
         if ( RTN_ALTER_CL_CRT_ID_IDX == taskType )
         {
            SINT64 contextID = -1 ;
            BSONObj def = BSON( FIELD_NAME_INDEX
                                << BSON( IXM_FIELD_NAME_KEY <<
                                         BSON( DMS_ID_KEY_NAME << 1 ) <<
                                         IXM_FIELD_NAME_NAME << IXM_ID_KEY_NAME
                                         << IXM_FIELD_NAME_UNIQUE <<
                                         true << IXM_FIELD_NAME_V << 0 <<
                                         IXM_FIELD_NAME_ENFORCED << true ) );
            rc = _createIndexOnMainCL( "", job.getName(),
                                       def.objdata(), NULL,
                                       1, contextID, TRUE ) ;
         }
         else if ( RTN_ALTER_CL_DROP_ID_IDX == taskType )
         {
            SINT64 contextID = -1 ;
            BSONObj def = BSON( FIELD_NAME_INDEX <<
                                BSON( IXM_FIELD_NAME_NAME
                                      << IXM_ID_KEY_NAME ) ) ;
            rc = _dropIndexOnMainCL( "", job.getName(),
                                     def.objdata(),
                                     1, contextID, TRUE ) ;
         }
         else
         {
            PD_LOG( PDERROR, "unknown task type:%d", taskType ) ;
            rc = SDB_SYS ;
            goto error ;
         }
      }
   done:
      return rc ;
   error:
      goto done ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__CKPRIMARYSTATUS, "_clsShdSession::_checkPrimary" )
   INT32 _clsShdSession::_checkPrimaryStatus()
   {
      INT32 rc = SDB_OK ;
      PD_TRACE_ENTRY( SDB__CLSSHDSESS__CKPRIMARYSTATUS ) ;
      UINT32 waitTime = 0 ;
      while( TRUE )
      {
         rc = _pReplSet->primaryCheck( _pEDUCB ) ;
         if ( SDB_OK == rc )
         {
            break ;
         }
         else if ( SDB_CLS_NOT_PRIMARY != rc )
         {
            goto error ;
         }
         else if ( MSG_INVALID_ROUTEID !=
                  ( _primaryID.value = _pReplSet->getPrimary().value ) &&
                  _pReplSet->isSendNormal( _primaryID.value ) )
         {
            rc = SDB_CLS_NOT_PRIMARY ;
            goto error ;
         }
         else if ( !CLS_IS_MAJORITY( _pReplSet->getAlivesByTimeout(),
                                     _pReplSet->groupSize() ) )
         {
            rc = SDB_CLS_NOT_PRIMARY ;
            goto error ;
         }
         else if ( waitTime < SHD_NOTPRIMARY_WAITTIME &&
                   !_pEDUCB->isInterrupted() )
         {
            INT32 result = SDB_OK ;
            rc = _pReplSet->getFaultEvent()->wait( SHD_WAITTIME_INTERVAL,
                                                   &result ) ;
            if ( SDB_OK == rc && SDB_OK != result )
            {
               rc = result ;
               goto error;
            }

            rc = SDB_OK ;
            waitTime += SHD_WAITTIME_INTERVAL ;
            continue ;
         }
         else
         {
            goto error ;
         }
      }
   done:
      PD_TRACE_EXITRC( SDB__CLSSHDSESS__CKPRIMARYSTATUS, rc ) ;
      return rc ;
   error:
      goto done ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__CKRBSTATUS, "_clsShdSession::_checkRollbackStatus" )
   INT32 _clsShdSession::_checkRollbackStatus()
   {
      INT32 rc = SDB_OK ;
      PD_TRACE_ENTRY( SDB__CLSSHDSESS__CKRBSTATUS ) ;
      UINT32 waitTime = 0 ;
      while( TRUE )
      {
         if ( !pmdGetKRCB()->getTransCB()->isDoRollback() )
         {
            if ( waitTime > 0 && _pEDUCB->isInterrupted() )
            {
               rc = SDB_APP_INTERRUPT ;
               goto error ;
            }
            break ;
         }
         else if ( waitTime < SHD_TRANSROLLBACK_WAITTIME &&
                   !_pEDUCB->isInterrupted() )
         {
            ossSleep( SHD_WAITTIME_INTERVAL ) ;
            waitTime += SHD_WAITTIME_INTERVAL ;
            continue ;
         }

         rc = SDB_DPS_TRANS_DOING_ROLLBACK ;
         goto error ;
      }
   done:
      PD_TRACE_EXITRC( SDB__CLSSHDSESS__CKRBSTATUS, rc ) ;
      return rc ;
   error:
      goto done ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__CKWRITESTATUS, "_clsShdSession::_checkWriteStatus" )
   INT32 _clsShdSession::_checkWriteStatus()
   {
      INT32 rc = SDB_OK ;
      PD_TRACE_ENTRY( SDB__CLSSHDSESS__CKWRITESTATUS ) ;
      rc = _checkPrimaryStatus() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDINFO, "failed to check primary status:%d", rc ) ;
         goto error ;
      }

      rc = _checkRollbackStatus() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDINFO, "failed to check rollback status:%d", rc ) ;
         goto error ;
      }

      _pEDUCB->writingDB( TRUE ) ;
   done:
      PD_TRACE_EXITRC( SDB__CLSSHDSESS__CKWRITESTATUS, rc ) ;
      return rc ;
   error:
      goto done ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__CKPRIMARYWHENREAD, "_clsShdSession::_checkPrimaryWhenRead" )
   INT32 _clsShdSession::_checkPrimaryWhenRead( INT32 flag, INT32 reqFlag )
   {
      INT32 rc = SDB_OK ;
      PD_TRACE_ENTRY( SDB__CLSSHDSESS__CKPRIMARYWHENREAD ) ;
      if ( flag & reqFlag )
      {
         rc = _checkPrimaryStatus() ;
         if ( SDB_OK != rc )
         {
            PD_LOG( PDINFO, "failed to check primary status:%d", rc ) ;
            goto error ;
         }
      }
   done:
      PD_TRACE_EXITRC( SDB__CLSSHDSESS__CKPRIMARYWHENREAD, rc ) ;
      return rc ;
   error:
      goto done ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__CKREPLSTATUS, "_clsShdSession::_checkReplStatus" )
   INT32 _clsShdSession::_checkReplStatus()
   {
      INT32 rc = SDB_OK ;
      PD_TRACE_ENTRY( SDB__CLSSHDSESS__CKREPLSTATUS ) ;

      if ( SDB_DB_FULLSYNC == PMD_DB_STATUS() )
      {
         rc = SDB_CLS_FULL_SYNC ;
      }
      else if ( SDB_DB_REBUILDING == PMD_DB_STATUS() )
      {
         rc = SDB_RTN_IN_REBUILD ;
      }

      PD_TRACE_EXITRC( SDB__CLSSHDSESS__CKREPLSTATUS, rc ) ;
      return rc ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__CHECKCLSANDGET, "_clsShdSession::_checkCLStatusAndGetSth" )
   INT32 _clsShdSession::_checkCLStatusAndGetSth( const CHAR *name,
                                                  INT32 version,
                                                  BOOLEAN *isMainCL,
                                                  INT16 *w )
   {
      INT32 rc = SDB_OK ;
      PD_TRACE_ENTRY( SDB__CLSSHDSESS__CHECKCLSANDGET ) ;
      INT32 curVer = -1 ;
      INT16 replSize = 0 ;
      UINT32 groupCount = 0 ;
      _clsCatalogSet *set = NULL ;
      BOOLEAN mainCL = FALSE ;
      BOOLEAN agentLocked = FALSE ;

      rc = _checkReplStatus() ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "failed to check status of repl-set:%d", rc ) ;
         goto error ;
      }

      _pCatAgent->lock_r () ;
      agentLocked = TRUE ;
      set = _pCatAgent->collectionSet( name ) ;
      if ( NULL == set )
      {
         rc = SDB_CLS_NO_CATALOG_INFO ;
         goto error ;
      }

      replSize = set->getW() ;
      curVer = set->getVersion() ;
      groupCount = set->groupCount() ;
      mainCL = set->isMainCL();
      _pCatAgent->release_r () ;
      agentLocked = FALSE ;

      if ( curVer < 0 )
      {
         rc = SDB_CLS_NO_CATALOG_INFO ;
         goto error ;
      }
      else if ( curVer < version )
      {
         rc = SDB_CLS_DATA_NODE_CAT_VER_OLD ;
         goto error ;
      }
      else if ( curVer > version
                || ( 0 == groupCount && !mainCL ) )
      {
         if ( 0 == groupCount )
         {
            _pCatAgent->lock_w() ;
            _pCatAgent->clear( name ) ;
            _pCatAgent->release_w() ;
         }
         PD_LOG ( PDINFO, "Collecton[%s]: self verions:%d, coord version:%d, "
                  "groupCount:%d", name, curVer, version, groupCount ) ;
         rc = SDB_CLS_COORD_NODE_CAT_VER_OLD ;
         goto error ;
      }
      else
      {
         if ( NULL != isMainCL )
         {
            *isMainCL = mainCL ;
         }

         if ( NULL != w )
         {
            *w = replSize ;
         }
      }
   done:
      if ( agentLocked )
      {
         _pCatAgent->release_r () ;
      }
      PD_TRACE_EXITRC( SDB__CLSSHDSESS__CHECKCLSANDGET, rc ) ;
      return rc ;
   error:
      goto done ;
   }

   // PD_TRACE_DECLARE_FUNCTION ( SDB__CLSSHDSESS__CALCW, "_clsShdSession::_calculateW" )
   INT32 _clsShdSession::_calculateW( const INT16 *replSize,
                                      const INT16 *clientW,
                                      INT16 &final )
   {
      INT32 rc = SDB_OK ;
      PD_TRACE_ENTRY( SDB__CLSSHDSESS__CALCW ) ;
      UINT32 N = 0 ; /// node count
      UINT32 A = 0 ; /// alive count
      INT16 w = 0 ;
      _pReplSet->getBoth( N, A ) ;

      if ( NULL != replSize )
      {
         w = ( NULL == clientW || 0 == *clientW ) ?
                       *replSize : *clientW ;
      }
      else if ( 0 == *clientW )
      {
         w = 1 ;
      }
      else
      {
         w = *clientW ;
      }

      if ( 1 <= w &&
           w <= CLS_REPLSET_MAX_NODE_SIZE )
      {
         w = w <= ( INT16 )N ? w: ( INT16 )N ;
      }
      else if ( -1 == w )
      {
         w = A ;
      }
      else if ( 0 == w )
      {
         w = N ;
      }
      else
      {
         stringstream ss ;
         ss << "node size[" << N << "],"
            << "alive size[" << A << "]," ;
         if ( NULL != replSize )
         {
            ss << "repl size[" << *replSize << "]," ;
         }
         if ( NULL != clientW )
         {
            ss << "client w[" << *clientW << "]" ;
         }
         PD_LOG( PDERROR, "can not calculate w:%s",
                 ss.str().c_str() ) ;
         rc = SDB_INVALIDARG ;
         goto error ;
      }

      SDB_ASSERT( 1 <= w && w <= CLS_REPLSET_MAX_NODE_SIZE, "must be valid" ) ;
      w = w <= 0 ? 1 : w ;
      if ( ( INT16 )A < w )
      {
         PD_LOG( PDERROR, "alive num[%d] can not meet need[%d]",
                 A, w ) ;
         rc = SDB_CLS_NODE_NOT_ENOUGH ;
         goto error ;
      }

      final = w ;
   done:
      PD_TRACE_EXITRC( SDB__CLSSHDSESS__CALCW, rc ) ;
      return rc ;
   error:
      goto done ;
   }
}
