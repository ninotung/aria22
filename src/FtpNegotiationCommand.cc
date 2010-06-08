/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "FtpNegotiationCommand.h"

#include <stdint.h>
#include <cassert>
#include <utility>
#include <map>

#include "Request.h"
#include "DownloadEngine.h"
#include "FtpConnection.h"
#include "RequestGroup.h"
#include "PieceStorage.h"
#include "FtpDownloadCommand.h"
#include "FileEntry.h"
#include "DlAbortEx.h"
#include "message.h"
#include "prefs.h"
#include "util.h"
#include "Option.h"
#include "Logger.h"
#include "Segment.h"
#include "DownloadContext.h"
#include "DefaultBtProgressInfoFile.h"
#include "RequestGroupMan.h"
#include "DownloadFailureException.h"
#include "Socket.h"
#include "StringFormat.h"
#include "DiskAdaptor.h"
#include "SegmentMan.h"
#include "AuthConfigFactory.h"
#include "AuthConfig.h"
#include "a2functional.h"
#include "URISelector.h"
#include "HttpConnection.h"
#include "HttpHeader.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "DlRetryEx.h"
#include "CookieStorage.h"
#include "ServerStatMan.h"
#include "FileAllocationEntry.h"
#include "CheckIntegrityEntry.h"

namespace aria2 {

FtpNegotiationCommand::FtpNegotiationCommand
(cuid_t cuid,
 const SharedHandle<Request>& req,
 const SharedHandle<FileEntry>& fileEntry,
 RequestGroup* requestGroup,
 DownloadEngine* e,
 const SocketHandle& s,
 Seq seq,
 const std::string& baseWorkingDir):
  AbstractCommand(cuid, req, fileEntry, requestGroup, e, s), sequence(seq),
  ftp(new FtpConnection(cuid, socket, req,
                        e->getAuthConfigFactory()->createAuthConfig
                        (req, requestGroup->getOption().get()),
                        getOption().get()))
{
  ftp->setBaseWorkingDir(baseWorkingDir);
  if(seq == SEQ_RECV_GREETING) {
    setTimeout(getOption()->getAsInt(PREF_CONNECT_TIMEOUT));
  }
  disableReadCheckSocket();
  setWriteCheckSocket(socket);
}

FtpNegotiationCommand::~FtpNegotiationCommand() {}

bool FtpNegotiationCommand::executeInternal() {
  while(processSequence(_segments.front()));
  if(sequence == SEQ_RETRY) {
    return prepareForRetry(0);
  } else if(sequence == SEQ_NEGOTIATION_COMPLETED) {
    FtpDownloadCommand* command =
      new FtpDownloadCommand
      (cuid, req, _fileEntry, _requestGroup, ftp, e, dataSocket, socket);
    command->setStartupIdleTime(getOption()->getAsInt(PREF_STARTUP_IDLE_TIME));
    command->setLowestDownloadSpeedLimit(getOption()->getAsInt(PREF_LOWEST_SPEED_LIMIT));
    if(!_fileEntry->isSingleHostMultiConnectionEnabled()) {
      _fileEntry->removeURIWhoseHostnameIs(req->getHost());
    }
    _requestGroup->getURISelector()->tuneDownloadCommand
      (_fileEntry->getRemainingUris(), command);
    e->addCommand(command);
    return true;
  } else if(sequence == SEQ_HEAD_OK || sequence == SEQ_DOWNLOAD_ALREADY_COMPLETED) {
    return true;
  } else if(sequence == SEQ_FILE_PREPARATION) {
    if(getOption()->getAsBool(PREF_FTP_PASV)) {
      sequence = SEQ_SEND_PASV;
    } else {
      sequence = SEQ_PREPARE_SERVER_SOCKET;
    }
    return false;
  } else if(sequence == SEQ_EXIT) {
    return true;
  } else {
    e->addCommand(this);
    return false;
  }
}

bool FtpNegotiationCommand::recvGreeting() {
  if(!checkIfConnectionEstablished
     (socket, _connectedHostname, _connectedAddr, _connectedPort)) {
    sequence = SEQ_EXIT;
    return false;
  }
  setTimeout(_requestGroup->getTimeout());
  //socket->setBlockingMode();
  disableWriteCheckSocket();
  setReadCheckSocket(socket);

  unsigned int status = ftp->receiveResponse();
  if(status == 0) {
    return false;
  }
  if(status != 220) {
    throw DL_ABORT_EX(EX_CONNECTION_FAILED);
  }
  sequence = SEQ_SEND_USER;

  return true;
}

bool FtpNegotiationCommand::sendUser() {
  if(ftp->sendUser()) {
    disableWriteCheckSocket();
    sequence = SEQ_RECV_USER;
  } else {
    setWriteCheckSocket(socket);
  }
  return false;
}

bool FtpNegotiationCommand::recvUser() {
  unsigned int status = ftp->receiveResponse();
  switch(status) {
  case 0:
    return false;
  case 230:
    sequence = SEQ_SEND_TYPE;
    break;
  case 331:
    sequence = SEQ_SEND_PASS;
    break;
  default:
    throw DL_ABORT_EX(StringFormat(EX_BAD_STATUS, status).str());
  }
  return true;
}

bool FtpNegotiationCommand::sendPass() {
  if(ftp->sendPass()) {
    disableWriteCheckSocket();
    sequence = SEQ_RECV_PASS;
  } else {
    setWriteCheckSocket(socket);
  }
  return false;
}

bool FtpNegotiationCommand::recvPass() {
  unsigned int status = ftp->receiveResponse();
  if(status == 0) {
    return false;
  }
  if(status != 230) {
    throw DL_ABORT_EX(StringFormat(EX_BAD_STATUS, status).str());
  }
  sequence = SEQ_SEND_TYPE;
  return true;
}

bool FtpNegotiationCommand::sendType() {
  if(ftp->sendType()) {
    disableWriteCheckSocket();
    sequence = SEQ_RECV_TYPE;
  } else {
    setWriteCheckSocket(socket);
  }
  return false;
}

bool FtpNegotiationCommand::recvType() {
  unsigned int status = ftp->receiveResponse();
  if(status == 0) {
    return false;
  }
  if(status != 200) {
    throw DL_ABORT_EX(StringFormat(EX_BAD_STATUS, status).str());
  }
  sequence = SEQ_SEND_PWD;
  return true;
}

bool FtpNegotiationCommand::sendPwd()
{
  if(ftp->sendPwd()) {
    disableWriteCheckSocket();
    sequence = SEQ_RECV_PWD;
  } else {
    setWriteCheckSocket(socket);
  }
  return false;
}

bool FtpNegotiationCommand::recvPwd()
{
  std::string pwd;
  unsigned int status = ftp->receivePwdResponse(pwd);
  if(status == 0) {
    return false;
  }
  if(status != 257) {
    throw DL_ABORT_EX(StringFormat(EX_BAD_STATUS, status).str());
  }
  ftp->setBaseWorkingDir(pwd);
  if(logger->info()) {
    logger->info("CUID#%s - base working directory is '%s'",
                 util::itos(cuid).c_str(), pwd.c_str());
  }
  sequence = SEQ_SEND_CWD;
  return true;
}

bool FtpNegotiationCommand::sendCwd() {
  // Calling setReadCheckSocket() is needed when the socket is reused, 
  setReadCheckSocket(socket);
  if(ftp->sendCwd()) {
    disableWriteCheckSocket();
    sequence = SEQ_RECV_CWD;
  } else {
    setWriteCheckSocket(socket);
  }
  return false;
}

bool FtpNegotiationCommand::recvCwd() {
  unsigned int status = ftp->receiveResponse();
  if(status == 0) {
    return false;
  }
  if(status != 250) {
    poolConnection();
    _requestGroup->increaseAndValidateFileNotFoundCount();
    if (status == 550)
      throw DL_ABORT_EX2(MSG_RESOURCE_NOT_FOUND,
                         downloadresultcode::RESOURCE_NOT_FOUND);
    else
      throw DL_ABORT_EX(StringFormat(EX_BAD_STATUS, status).str());
  }
  if(getOption()->getAsBool(PREF_REMOTE_TIME)) {
    sequence = SEQ_SEND_MDTM;
  } else {
    sequence = SEQ_SEND_SIZE;
  }
  return true;
}

bool FtpNegotiationCommand::sendMdtm()
{
  if(ftp->sendMdtm()) {
    disableWriteCheckSocket();
    sequence = SEQ_RECV_MDTM;
  } else {
    setWriteCheckSocket(socket);
  }
  return false;
}

bool FtpNegotiationCommand::recvMdtm()
{
  Time lastModifiedTime = Time::null();
  unsigned int status = ftp->receiveMdtmResponse(lastModifiedTime);
  if(status == 0) {
    return false;
  }
  if(status == 213) {
    if(lastModifiedTime.good()) {
      _requestGroup->updateLastModifiedTime(lastModifiedTime);
      time_t t = lastModifiedTime.getTime();
      struct tm* tms = gmtime(&t); // returned struct is statically allocated.
      if(tms) {
        if(logger->debug()) {
          logger->debug("MDTM result was parsed as: %s GMT", asctime(tms));
        }
      } else {
        if(logger->debug()) {
          logger->debug("gmtime() failed for MDTM result.");
        }
      }
    } else {
      if(logger->debug()) {
        logger->debug("MDTM response was returned, but it seems not to be a"
                      " time value as in specified in RFC3659.");
      }
    }
  } else {
    if(logger->info()) {
      logger->info("CUID#%s - MDTM command failed.", util::itos(cuid).c_str());
    }
  }
  sequence = SEQ_SEND_SIZE;
  return true;  
}

bool FtpNegotiationCommand::sendSize() {
  if(ftp->sendSize()) {
    disableWriteCheckSocket();
    sequence = SEQ_RECV_SIZE;
  } else {
    setWriteCheckSocket(socket);
  }
  return false;
}

bool FtpNegotiationCommand::onFileSizeDetermined(uint64_t totalLength)
{
  _fileEntry->setLength(totalLength);
  if(_fileEntry->getPath().empty()) {
    _fileEntry->setPath
      (util::applyDir
       (getDownloadContext()->getDir(),
        util::fixTaintedBasename(util::percentDecode(req->getFile()))));
  }
  _requestGroup->preDownloadProcessing();
  if(e->getRequestGroupMan()->isSameFileBeingDownloaded(_requestGroup)) {
    throw DOWNLOAD_FAILURE_EXCEPTION
      (StringFormat(EX_DUPLICATE_FILE_DOWNLOAD,
                    _requestGroup->getFirstFilePath().c_str()).str());
  }
  if(totalLength == 0) {

    if(getOption()->getAsBool(PREF_FTP_PASV)) {
      sequence = SEQ_SEND_PASV;
    } else {
      sequence = SEQ_PREPARE_SERVER_SOCKET;
    }

    if(getOption()->getAsBool(PREF_DRY_RUN)) {
      _requestGroup->initPieceStorage();
      onDryRunFileFound();
      return false;
    }

    if(_requestGroup->downloadFinishedByFileLength()) {
      _requestGroup->initPieceStorage();
      _requestGroup->getPieceStorage()->markAllPiecesDone();
      sequence = SEQ_DOWNLOAD_ALREADY_COMPLETED;

      logger->notice(MSG_DOWNLOAD_ALREADY_COMPLETED,
                     util::itos(_requestGroup->getGID()).c_str(),
                     _requestGroup->getFirstFilePath().c_str());

      poolConnection();

      return false;
    }

    _requestGroup->shouldCancelDownloadForSafety();
    _requestGroup->initPieceStorage();
    _requestGroup->getPieceStorage()->getDiskAdaptor()->initAndOpenFile();

    if(getDownloadContext()->knowsTotalLength()) {
      sequence = SEQ_DOWNLOAD_ALREADY_COMPLETED;
      poolConnection();
      return false;
    }
    // We have to make sure that command that has Request object must
    // have segment after PieceStorage is initialized. See
    // AbstractCommand::execute()
    _requestGroup->getSegmentMan()->getSegment(cuid, 0);
    return true;
  } else {
    _requestGroup->adjustFilename
      (SharedHandle<BtProgressInfoFile>(new DefaultBtProgressInfoFile
                                        (_requestGroup->getDownloadContext(),
                                         SharedHandle<PieceStorage>(),
                                         getOption().get())));
    _requestGroup->initPieceStorage();

    if(getOption()->getAsBool(PREF_DRY_RUN)) {
      onDryRunFileFound();
      return false;
    }

    BtProgressInfoFileHandle infoFile(new DefaultBtProgressInfoFile(_requestGroup->getDownloadContext(), _requestGroup->getPieceStorage(), getOption().get()));
    if(!infoFile->exists() && _requestGroup->downloadFinishedByFileLength()) {
      _requestGroup->getPieceStorage()->markAllPiecesDone();

      sequence = SEQ_DOWNLOAD_ALREADY_COMPLETED;
      
      logger->notice(MSG_DOWNLOAD_ALREADY_COMPLETED,
                     util::itos(_requestGroup->getGID()).c_str(),
                     _requestGroup->getFirstFilePath().c_str());

      poolConnection();
      
      return false;
    }
    _requestGroup->loadAndOpenFile(infoFile);
    // We have to make sure that command that has Request object must
    // have segment after PieceStorage is initialized. See
    // AbstractCommand::execute()
    _requestGroup->getSegmentMan()->getSegment(cuid, 0);

    prepareForNextAction(this);

    disableReadCheckSocket();
  }
  return false;
}

bool FtpNegotiationCommand::recvSize() {
  uint64_t size = 0;
  unsigned int status = ftp->receiveSizeResponse(size);
  if(status == 0) {
    return false;
  }
  if(status == 213) {

    if(size > INT64_MAX) {
      throw DL_ABORT_EX
        (StringFormat(EX_TOO_LARGE_FILE, util::uitos(size, true).c_str()).str());
    }
    if(_requestGroup->getPieceStorage().isNull()) {

      sequence = SEQ_FILE_PREPARATION;
      return onFileSizeDetermined(size);

    } else {
      _requestGroup->validateTotalLength(_fileEntry->getLength(), size);
    }

  } else {
    if(logger->info()) {
      logger->info("CUID#%s - The remote FTP Server doesn't recognize SIZE"
                   " command. Continue.", util::itos(cuid).c_str());
    }
    // Even if one of the other servers waiting in the queue supports SIZE
    // command, resuming and segmented downloading are disabled when the first
    // contacted FTP server doesn't support it.
    if(_requestGroup->getPieceStorage().isNull()) {
      getDownloadContext()->markTotalLengthIsUnknown();
      return onFileSizeDetermined(0);

    }
    // TODO Skipping RequestGroup::validateTotalLength(0) here will allow
    // wrong file to be downloaded if user-specified URL is wrong.
  }
  if(getOption()->getAsBool(PREF_FTP_PASV)) {
    sequence = SEQ_SEND_PASV;
  } else {
    sequence = SEQ_PREPARE_SERVER_SOCKET;
  }
  return true;
}

void FtpNegotiationCommand::afterFileAllocation()
{
  setReadCheckSocket(socket);
}

bool FtpNegotiationCommand::prepareServerSocket()
{
  serverSocket = ftp->createServerSocket();
  sequence = SEQ_SEND_PORT;
  return true;
}

bool FtpNegotiationCommand::sendPort() {
  afterFileAllocation();
  if(ftp->sendPort(serverSocket)) {
    disableWriteCheckSocket();
    sequence = SEQ_RECV_PORT;
  } else {
    setWriteCheckSocket(socket);
  }
  return false;
}

bool FtpNegotiationCommand::recvPort() {
  unsigned int status = ftp->receiveResponse();
  if(status == 0) {
    return false;
  }
  if(status != 200) {
    throw DL_ABORT_EX(StringFormat(EX_BAD_STATUS, status).str());
  }
  sequence = SEQ_SEND_REST;
  return true;
}

bool FtpNegotiationCommand::sendPasv() {
  afterFileAllocation();
  if(ftp->sendPasv()) {
    disableWriteCheckSocket();
    sequence = SEQ_RECV_PASV;
  } else {
    setWriteCheckSocket(socket);
  }
  return false;
}

bool FtpNegotiationCommand::recvPasv() {
  std::pair<std::string, uint16_t> dest;
  unsigned int status = ftp->receivePasvResponse(dest);
  if(status == 0) {
    return false;
  }
  if(status != 227) {
    throw DL_ABORT_EX(StringFormat(EX_BAD_STATUS, status).str());
  }
  // TODO Should we check to see that dest.first is not in noProxy list?
  if(isProxyDefined()) {
    _dataConnAddr = dest;
    sequence = SEQ_RESOLVE_PROXY;
    return true;
  } else {
    // make a data connection to the server.
    if(logger->info()) {
      logger->info(MSG_CONNECTING_TO_SERVER, util::itos(cuid).c_str(),
                   dest.first.c_str(),
                   dest.second);
    }
    dataSocket.reset(new SocketCore());
    dataSocket->establishConnection(dest.first, dest.second);
    disableReadCheckSocket();
    setWriteCheckSocket(dataSocket);
    sequence = SEQ_SEND_REST_PASV;
    return false;
  }
}

bool FtpNegotiationCommand::resolveProxy()
{
  SharedHandle<Request> proxyReq = createProxyRequest();
  std::vector<std::string> addrs;
  _proxyAddr = resolveHostname
    (addrs, proxyReq->getHost(), proxyReq->getPort());
  if(_proxyAddr.empty()) {
    return false;
  }
  if(logger->info()) {
    logger->info(MSG_CONNECTING_TO_SERVER, util::itos(cuid).c_str(),
                 _proxyAddr.c_str(), proxyReq->getPort());
  }
  dataSocket.reset(new SocketCore());                  
  dataSocket->establishConnection(_proxyAddr, proxyReq->getPort());
  disableReadCheckSocket();
  setWriteCheckSocket(dataSocket);
  _http.reset(new HttpConnection(cuid, dataSocket, getOption().get()));
  sequence = SEQ_SEND_TUNNEL_REQUEST;
  return false;
}

bool FtpNegotiationCommand::sendTunnelRequest()
{
  if(_http->sendBufferIsEmpty()) {
    if(dataSocket->isReadable(0)) {
      std::string error = socket->getSocketError();
      if(!error.empty()) {
        SharedHandle<Request> proxyReq = createProxyRequest();
        e->markBadIPAddress(proxyReq->getHost(),_proxyAddr,proxyReq->getPort());
        std::string nextProxyAddr = e->findCachedIPAddress
          (proxyReq->getHost(), proxyReq->getPort());
        if(nextProxyAddr.empty()) {
          e->removeCachedIPAddress(proxyReq->getHost(), proxyReq->getPort());
          throw DL_RETRY_EX
            (StringFormat(MSG_ESTABLISHING_CONNECTION_FAILED,
                          error.c_str()).str());
        } else {
          if(logger->info()) {
            logger->info(MSG_CONNECT_FAILED_AND_RETRY,
                         util::itos(cuid).c_str(),
                         _proxyAddr.c_str(), proxyReq->getPort());
          }
          _proxyAddr = nextProxyAddr;
          if(logger->info()) {
            logger->info(MSG_CONNECTING_TO_SERVER, util::itos(cuid).c_str(),
                         _proxyAddr.c_str(), proxyReq->getPort());
          }
          dataSocket->establishConnection(_proxyAddr, proxyReq->getPort());
          return false;
        }
      }
    }      
    SharedHandle<HttpRequest> httpRequest(new HttpRequest());
    httpRequest->setUserAgent(getOption()->get(PREF_USER_AGENT));
    SharedHandle<Request> req(new Request());
    // Construct fake URI in order to use HttpRequest
    req->setUri(strconcat("ftp://", _dataConnAddr.first,
                          A2STR::COLON_C, util::uitos(_dataConnAddr.second)));
    httpRequest->setRequest(req);
    httpRequest->setProxyRequest(createProxyRequest());
    _http->sendProxyRequest(httpRequest);
  } else {
    _http->sendPendingData();
  }
  if(_http->sendBufferIsEmpty()) {
    disableWriteCheckSocket();
    setReadCheckSocket(dataSocket);
    sequence = SEQ_RECV_TUNNEL_RESPONSE;
    return false;
  } else {
    setWriteCheckSocket(dataSocket);
    return false;
  }
}

bool FtpNegotiationCommand::recvTunnelResponse()
{
  SharedHandle<HttpResponse> httpResponse = _http->receiveResponse();
  if(httpResponse.isNull()) {
    return false;
  }
  if(httpResponse->getResponseStatus() != HttpHeader::S200) {
    throw DL_RETRY_EX(EX_PROXY_CONNECTION_FAILED);
  }
  sequence = SEQ_SEND_REST_PASV;
  return true;
}

bool FtpNegotiationCommand::sendRestPasv(const SharedHandle<Segment>& segment) {
  //dataSocket->setBlockingMode();
  setReadCheckSocket(socket);
  disableWriteCheckSocket();
  return sendRest(segment);
}

bool FtpNegotiationCommand::sendRest(const SharedHandle<Segment>& segment) {
  if(ftp->sendRest(segment)) {
    disableWriteCheckSocket();
    sequence = SEQ_RECV_REST;
  } else {
    setWriteCheckSocket(socket);
  }
  return false;
}

bool FtpNegotiationCommand::recvRest(const SharedHandle<Segment>& segment) {
  unsigned int status = ftp->receiveResponse();
  if(status == 0) {
    return false;
  }
  // If we recieve negative response and requested file position is not 0,
  // then throw exception here.
  if(status != 350) {
    if(!segment.isNull() && segment->getPositionToWrite() != 0) {
      throw DL_ABORT_EX2("FTP server doesn't support resuming.",
                         downloadresultcode::CANNOT_RESUME);
    }
  }
  sequence = SEQ_SEND_RETR;
  return true;
}

bool FtpNegotiationCommand::sendRetr() {
  if(ftp->sendRetr()) {
    disableWriteCheckSocket();
    sequence = SEQ_RECV_RETR;
  } else {
    setWriteCheckSocket(socket);
  }
  return false;
}

bool FtpNegotiationCommand::recvRetr() {
  unsigned int status = ftp->receiveResponse();
  if(status == 0) {
    return false;
  }
  if(status != 150 && status != 125) {
    _requestGroup->increaseAndValidateFileNotFoundCount();
    if (status == 550)
      throw DL_ABORT_EX2(MSG_RESOURCE_NOT_FOUND,
                         downloadresultcode::RESOURCE_NOT_FOUND);
    else
      throw DL_ABORT_EX(StringFormat(EX_BAD_STATUS, status).str());
  }
  if(getOption()->getAsBool(PREF_FTP_PASV)) {
    sequence = SEQ_NEGOTIATION_COMPLETED;
    return false;
  } else {
    disableReadCheckSocket();
    setReadCheckSocket(serverSocket);
    sequence = SEQ_WAIT_CONNECTION;
    return false;
  }
}

bool FtpNegotiationCommand::waitConnection()
{
  disableReadCheckSocket();
  setReadCheckSocket(socket);
  dataSocket.reset(serverSocket->acceptConnection());
  dataSocket->setNonBlockingMode();
  sequence = SEQ_NEGOTIATION_COMPLETED;
  return false;
}

bool FtpNegotiationCommand::processSequence
(const SharedHandle<Segment>& segment) {
  bool doNextSequence = true;
  switch(sequence) {
  case SEQ_RECV_GREETING:
    return recvGreeting();
  case SEQ_SEND_USER:
    return sendUser();
  case SEQ_RECV_USER:
    return recvUser();
  case SEQ_SEND_PASS:
    return sendPass();
  case SEQ_RECV_PASS:
    return recvPass();
  case SEQ_SEND_TYPE:
    return sendType();
  case SEQ_RECV_TYPE:
    return recvType();
  case SEQ_SEND_PWD:
    return sendPwd();
  case SEQ_RECV_PWD:
    return recvPwd();
  case SEQ_SEND_CWD:
    return sendCwd();
  case SEQ_RECV_CWD:
    return recvCwd();
  case SEQ_SEND_MDTM:
    return sendMdtm();
  case SEQ_RECV_MDTM:
    return recvMdtm();
  case SEQ_SEND_SIZE:
    return sendSize();
  case SEQ_RECV_SIZE:
    return recvSize();
  case SEQ_PREPARE_SERVER_SOCKET:
    return prepareServerSocket();
  case SEQ_SEND_PORT:
    return sendPort();
  case SEQ_RECV_PORT:
    return recvPort();
  case SEQ_SEND_PASV:
    return sendPasv();
  case SEQ_RECV_PASV:
    return recvPasv();
  case SEQ_RESOLVE_PROXY:
    return resolveProxy();
  case SEQ_SEND_TUNNEL_REQUEST:
    return sendTunnelRequest();
  case SEQ_RECV_TUNNEL_RESPONSE:
    return recvTunnelResponse();
  case SEQ_SEND_REST_PASV:
    return sendRestPasv(segment);
  case SEQ_SEND_REST:
    return sendRest(segment);
  case SEQ_RECV_REST:
    return recvRest(segment);
  case SEQ_SEND_RETR:
    return sendRetr();
  case SEQ_RECV_RETR:
    return recvRetr();
  case SEQ_WAIT_CONNECTION:
    return waitConnection();
  default:
    abort();
  }
  return doNextSequence;
}

void FtpNegotiationCommand::poolConnection() const
{
  if(getOption()->getAsBool(PREF_FTP_REUSE_CONNECTION)) {
    std::map<std::string, std::string> options;
    options["baseWorkingDir"] = ftp->getBaseWorkingDir();
    e->poolSocket(req, ftp->getUser(), createProxyRequest(), socket, options);
  }
}

void FtpNegotiationCommand::onDryRunFileFound()
{
  _requestGroup->getPieceStorage()->markAllPiecesDone();
  poolConnection();
  sequence = SEQ_HEAD_OK;
}

} // namespace aria2
