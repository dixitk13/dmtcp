/****************************************************************************
 *   Copyright (C) 2006-2013 by Jason Ansel, Kapil Arya, and Gene Cooperman *
 *   jansel@csail.mit.edu, kapil@ccs.neu.edu, gene@ccs.neu.edu              *
 *                                                                          *
 *  This file is part of DMTCP.                                             *
 *                                                                          *
 *  DMTCP is free software: you can redistribute it and/or                  *
 *  modify it under the terms of the GNU Lesser General Public License as   *
 *  published by the Free Software Foundation, either version 3 of the      *
 *  License, or (at your option) any later version.                         *
 *                                                                          *
 *  DMTCP is distributed in the hope that it will be useful,                *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 *  GNU Lesser General Public License for more details.                     *
 *                                                                          *
 *  You should have received a copy of the GNU Lesser General Public        *
 *  License along with DMTCP:dmtcp/src.  If not, see                        *
 *  <http://www.gnu.org/licenses/>.                                         *
 ****************************************************************************/

// CAN REMOVE BOOL enableCheckpointing ARG OF DmtcpWorker WHEN WE'RE DONE.
// DmtcpWorker CAN INHERIT THIS CLASS, CoordinatorAPI

#include <netdb.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include "coordinatorapi.h"
#include "dmtcp.h"
#include "util.h"
#include "syscallwrappers.h"
#include "util.h"
#include "shareddata.h"
#include "processinfo.h"
#include  "../jalib/jconvert.h"
#include  "../jalib/jfilesystem.h"
#include "../jalib/jsocket.h"
#include <fcntl.h>
#include <semaphore.h> // for sem_post(&sem_launch)
#include <zookeeper/zookeeper.h>

// sem_launch is used in threadlist.cpp
// sem_launch_first_time will be set just before pthread_create(checkpointhread)
LIB_PRIVATE bool sem_launch_first_time = false;
LIB_PRIVATE sem_t sem_launch;

namespace dmtcp {

static void coordinatorAPI_EventHook(DmtcpEvent_t event, DmtcpEventData_t *data)
{
  if (CoordinatorAPI::noCoordinator()) return;
  switch (event) {
    case DMTCP_EVENT_INIT:
      CoordinatorAPI::instance().init();
      break;

    case DMTCP_EVENT_EXIT:
      JTRACE("exit() in progress, disconnecting from dmtcp coordinator");
      CoordinatorAPI::instance().closeConnection();
      break;

    default:
      break;
  }
}

static DmtcpBarrier coordinatorAPIBarriers[] = {
  {DMTCP_PRIVATE_BARRIER_RESTART, CoordinatorAPI::restart, "restart"}
};

static bool _firstTime = true;
static const char *_cachedHost = NULL;
static int _cachedPort = 0;
static string extraName;

/* zookeeper additions : START*/

static bool _firstTimeZoo = true;
static zhandle_t *zh;
static int connected;
char gpath[1024];
static char mycontext[] = "testingdata";

int master_number = 999;
char master_string[23];
struct String_vector myvector;
char master_znode_base_path[] = "/master";
char full_master_znode_base_path[] = "/master/";

char zoohost[255];
int zooport;
  // change
char buffer[255];
/* zookeeper additions : END*/


void CoordinatorAPI::getCoordHostAndPort(CoordinatorMode mode,
                                         const char **host, int *port)
{
  JTRACE("getting coordinator host and port");

  if (SharedData::initialized()) {
    JTRACE("getting coordinator host and port from shareddata");
    *host = SharedData::coordHost().c_str();
    *port = SharedData::coordPort();
    return;
  }

  if (_firstTime) {
    // Set host to cmd line (if --cord-host) or env var or DEFAULT_HOST
    JTRACE("taking host as default");
    if (*host == NULL) {
      if (getenv(ENV_VAR_NAME_HOST)) {
        *host = getenv(ENV_VAR_NAME_HOST);
      } else if (getenv("DMTCP_HOST")) { // deprecated
        *host = getenv("DMTCP_HOST");
      } else {
        *host = DEFAULT_HOST;
      }
    }

    // Set port to cmd line (if --coord-port) or env var
    //   or 0 (if --new-coordinator from cmd line) or DEFAULT_PORT
    if (*port == UNINITIALIZED_PORT) {
      JTRACE("un-init port, getting from environment");
      if (getenv(ENV_VAR_NAME_PORT)) {
        *port = jalib::StringToInt(getenv(ENV_VAR_NAME_PORT));
      } else if (getenv("DMTCP_PORT")) { // deprecated
        *port = jalib::StringToInt(getenv("DMTCP_PORT"));
      } else if (mode & COORD_NEW) {
        *port = 0;
      } else {
        *port = DEFAULT_PORT;
      }
    }

    _cachedHost = *host;
    _cachedPort = *port;
    _firstTime = false;

  } else {
    // We might have gotten a user-requested port of 0 (random port) before,
    //   and now the user is passing in the actual coordinator port.
    if (*port > 0 && _cachedPort == 0) {
      _cachedPort = *port;
    }
    *host = _cachedHost;
    *port = _cachedPort;
  }
}

void CoordinatorAPI::setCoordPort(int port)
{
  _cachedPort = port;
}

static DmtcpPluginDescriptor_t coordinatorAPIPlugin = {
  DMTCP_PLUGIN_API_VERSION,
  PACKAGE_VERSION,
  "coordinatorapi",
  "DMTCP",
  "dmtcp@ccs.neu.edu",
  "Coordinator API plugin",
  DMTCP_DECL_BARRIERS(coordinatorAPIBarriers),
  coordinatorAPI_EventHook
};

DmtcpPluginDescriptor_t dmtcp_CoordinatorAPI_PluginDescr()
{
  return coordinatorAPIPlugin;
}

void CoordinatorAPI::restart()
{
  _real_close(instance()._nsSock);
  instance()._nsSock = -1;
}

static uint32_t getCkptInterval()
{
  uint32_t ret = DMTCPMESSAGE_SAME_CKPT_INTERVAL;
  const char* interval = getenv (ENV_VAR_CKPT_INTR);
  /* DmtcpMessage constructor default:
   *   hello_local.theCheckpointInterval: DMTCPMESSAGE_SAME_CKPT_INTERVAL
   */
  if (interval != NULL) {
    ret = jalib::StringToInt (interval);
  }
  // Tell the coordinator the ckpt interval only once.  It can change later.
  _dmtcp_unsetenv (ENV_VAR_CKPT_INTR);
  return ret;
}

static int createNewSocketToCoordinator(CoordinatorMode mode)
{
  const char*host = NULL;
  int port = UNINITIALIZED_PORT;
  JTRACE("Creating new Socket to coord with port ")(&port);

  CoordinatorAPI::getCoordHostAndPort(COORD_ANY, &host, &port);
  return jalib::JClientSocket(host, port).sockfd();
}

static int createNewSocketToNewCoordinator()
{
  // const char*host = NULL;
  // int port = 6669;
  // JTRACE("Creating new Socket to coord with port ")(&port);

  // // CoordinatorAPI::getCoordHostAndPort(COORD_ANY, &host, &port);
  // return jalib::JClientSocket(host, port).sockfd();

  JTRACE("Creating new Socket to coord with port ")(&zooport);

  // CoordinatorAPI::getCoordHostAndPort(COORD_ANY, &host, &port);
  printf("new host %s new port %d\n",zoohost, zooport );
  char host1[] = "127.0.0.1";
  // strncpy(zoohost, "127.0.0.1", sizeof(host1) );
    printf("new host %s new port %d\n",zoohost, zooport );
  return jalib::JClientSocket(zoohost, zooport).sockfd();
}


//CoordinatorAPI::CoordinatorAPI (int sockfd)
  //: _coordinatorSocket(sockfd)
//{ }

static CoordinatorAPI *coordAPIInst = NULL;
CoordinatorAPI& CoordinatorAPI::instance()
{
  //static SysVIPC *inst = new SysVIPC(); return *inst;
  if (coordAPIInst == NULL) {
    coordAPIInst = new CoordinatorAPI();
    if (noCoordinator() ||
        Util::isValidFd(PROTECTED_COORD_FD)) {
      coordAPIInst->_coordinatorSocket = PROTECTED_COORD_FD;
    }
  }
  return *coordAPIInst;
}

void CoordinatorAPI::init()
{
  JTRACE("Informing coordinator of new process") (UniquePid::ThisProcess());

  DmtcpMessage msg (DMT_UPDATE_PROCESS_INFO_AFTER_INIT_OR_EXEC);
  instance().sendMsgToCoordinator(msg, jalib::Filesystem::GetProgramName());
}

void CoordinatorAPI::resetOnFork(CoordinatorAPI& coordAPI)
{
  JASSERT(coordAPI._coordinatorSocket != -1);
  JASSERT(coordAPI._coordinatorSocket != PROTECTED_COORD_FD);
  coordAPI.updateSockFd();
  instance() = coordAPI;

  JTRACE("Informing coordinator of new process") (UniquePid::ThisProcess());

  DmtcpMessage msg (DMT_UPDATE_PROCESS_INFO_AFTER_FORK);
  if (dmtcp_virtual_to_real_pid) {
    msg.realPid = dmtcp_virtual_to_real_pid(getpid());
  } else {
    msg.realPid = getpid();
  }
  instance().sendMsgToCoordinator(msg);
  _real_close(instance()._nsSock);
  instance()._nsSock = -1;
}

void CoordinatorAPI::updateSockFd()
{
  _coordinatorSocket = Util::changeFd(_coordinatorSocket, PROTECTED_COORD_FD);
  JASSERT(_coordinatorSocket == PROTECTED_COORD_FD);
}

void CoordinatorAPI::connectAndSendUserCommand(char c,
                                               int *coordCmdStatus,
                                               int *numPeers,
                                               int *isRunning,
                                               int *ckptInterval)
{
  _coordinatorSocket = createNewSocketToCoordinator(COORD_ANY);
  if (_coordinatorSocket == -1) {
    *coordCmdStatus = CoordCmdStatus::ERROR_COORDINATOR_NOT_FOUND;
    return;
  }

  //tell the coordinator to run given user command
  DmtcpMessage msg, reply;

  //send
  msg.type = DMT_USER_CMD;
  msg.coordCmd = c;

  if (c == 'i') {
    const char* interval = getenv (ENV_VAR_CKPT_INTR);
    if (interval != NULL){
      msg.theCheckpointInterval = jalib::StringToInt (interval);
    }
  }

  sendMsgToCoordinator(msg);

  //the coordinator will violently close our socket...
  if (c=='q' || c=='Q') {
    *coordCmdStatus = CoordCmdStatus::NOERROR;
    return;
  }

  //receive REPLY
  recvMsgFromCoordinator(&reply);
  JASSERT_STDERR << "\tinside connectAndSendUserCommand!\n";

  reply.assertValid();
  JASSERT(reply.type == DMT_USER_CMD_RESULT);

  if (coordCmdStatus != NULL) {
    *coordCmdStatus =  reply.coordCmdStatus;
  }
  if (numPeers != NULL) {
    *numPeers =  reply.numPeers;
  }
  if (isRunning != NULL) {
    *isRunning = reply.isRunning;
  }
  if (ckptInterval != NULL) {
    *ckptInterval = reply.theCheckpointInterval;
  }

  _real_close(_coordinatorSocket);
  _coordinatorSocket = -1;
}

string CoordinatorAPI::getCoordCkptDir(void)
{
  // FIXME: Add a test for make-check.
  char buf[PATH_MAX];
  if (noCoordinator()) return "";
  DmtcpMessage msg(DMT_GET_CKPT_DIR);
  sendMsgToCoordinator(msg);
  JASSERT_STDERR << "\tinside getCoordCkptDir1!\n";
  char *extraData = NULL;
  recvMsgFromCoordinator(&msg, (void**)&extraData);
  JASSERT_STDERR << "\tinside getCoordCkptDir2!\n";
  msg.assertValid();
  JASSERT(msg.type == DMT_GET_CKPT_DIR_RESULT) (msg.type);

  JASSERT(msg.extraBytes > 0 && msg.extraBytes < PATH_MAX);
  strcpy(buf, extraData);
  JALLOC_HELPER_FREE(extraData);
  return buf;
}

void CoordinatorAPI::updateCoordCkptDir(const char *dir)
{
  if (noCoordinator()) return;
  JASSERT(dir != NULL);
  DmtcpMessage msg(DMT_UPDATE_CKPT_DIR);
  sendMsgToCoordinator(msg, dir, strlen(dir) + 1);
}

void CoordinatorAPI::sendMsgToCoordinator(DmtcpMessage msg,
                                          const void *extraData,
                                          size_t len)
{printf("method : send msg to coordinator \n");
  if (noCoordinator()) return;
  if (extraData != NULL) {
    msg.extraBytes = len;
  }
  JASSERT(Util::writeAll(_coordinatorSocket, &msg, sizeof(msg)) == sizeof(msg));
  if (extraData != NULL) {
    JASSERT(Util::writeAll(_coordinatorSocket, extraData, len) == len);
  }
}

void CoordinatorAPI::sendMsgToCoordinator(const DmtcpMessage &msg,
                                          const string &data)
{
  sendMsgToCoordinator(msg, data.c_str(), data.length() + 1);
}

void CoordinatorAPI::recvBroadCastMsgFromCoord(DmtcpMessage *msg)
{
  Util::readAll(_coordinatorSocket, msg, sizeof(*msg));
  return;
}

void CoordinatorAPI::recvMsgFromCoordinator(DmtcpMessage *msg, void **extraData)
{
  JASSERT_STDERR << "\tinside recvMsgFromCoordinator!\n";
  msg->poison();
  JASSERT(!noCoordinator()).Text("internal error");
  JASSERT_STDERR << "\tinside recvMsgFromCoordinator 1!\n";
  if (sem_launch_first_time) {
    JASSERT_STDERR << "\tinside recvMsgFromCoordinator1.1 !\n";
    // Release user thread now that we've initialized the checkpoint thread.
    // This code is reached if the --no-coordinator flag is not used.
    // FIXME:  Technically, some rare type of software could still execute
    //   between here and when we readall() from coord, thus creating a race.
    sem_post(&sem_launch);
    sem_launch_first_time = false;
  }
  JASSERT_STDERR << "\tinside recvMsgFromCoordinator 2!\n";
  if (Util::readAll(_coordinatorSocket, msg, sizeof(*msg)) != sizeof(*msg)) {
    // Perhaps the process is exit()'ing.
    JASSERT_STDERR << "inside readign incorrect socket data!\n";
    return;
  }
  JASSERT_STDERR << "\tinside recvMsgFromCoordinator 3\n";

  // if some other coordinator

  if (msg->extraBytes > 0) {
    JASSERT_STDERR << "\tinside recvMsgFromCoordinator 4!\n";
    JASSERT(extraData != NULL);
    // Caller must free this buffer
    void *buf = JALLOC_HELPER_MALLOC(msg->extraBytes);
    JASSERT(Util::readAll(_coordinatorSocket, (char*) buf, msg->extraBytes) ==
            msg->extraBytes);
    JASSERT(extraData != NULL);
    *extraData = buf;
  }
  JASSERT_STDERR << "\texiting recvMsgFromCoordinator completed!\n";
}

void CoordinatorAPI::waitForBarrier(const string& barrierId)
{
  instance().sendMsgToCoordinator(DmtcpMessage(DMT_OK));

  JTRACE("waiting for DMT_BARRIER_RELEASED message");

  char *extraData = NULL;
  DmtcpMessage msg;

  instance().recvMsgFromCoordinator(&msg, (void**)&extraData);
  JASSERT_STDERR << "\t inside waitForBarrier\n";
  msg.assertValid();
  if (msg.type == DMT_KILL_PEER) {
    JTRACE("Received KILL message from coordinator, exiting");
    JASSERT_STDERR << "Received KILL message from coordinator, exiting waitForBarrier!\n";
    // here print
    _exit (0);
  }

  JASSERT(msg.type == DMT_BARRIER_RELEASED) (msg.type);
  JASSERT(extraData != NULL);
  JASSERT(barrierId == extraData) (barrierId) (extraData);

  JALLOC_FREE(extraData);

}

void CoordinatorAPI::startNewCoordinator(CoordinatorMode mode)
{
  const char *host;
  int port;
  CoordinatorAPI::getCoordHostAndPort(mode, &host, &port);

  JASSERT(strcmp(host, "localhost") == 0 ||
          strcmp(host, "127.0.0.1") == 0 ||
          jalib::Filesystem::GetCurrentHostname() == host)
    (host) (jalib::Filesystem::GetCurrentHostname())
    .Text("Won't automatically start coordinator because DMTCP_HOST"
          " is set to a remote host.");
  // Create a socket and bind it to an unused port.
  errno = 0;
  jalib::JServerSocket coordinatorListenerSocket(jalib::JSockAddr::ANY,
                                                 port, 128);
  JASSERT(coordinatorListenerSocket.isValid())
    (coordinatorListenerSocket.port()) (JASSERT_ERRNO)
    .Text("Failed to create listen socket."
          "\nIf msg is \"Address already in use\", this may be an old coordinator."
          "\nKill other coordinators and try again in a minute or so.");
  // Now dup the sockfd to
  coordinatorListenerSocket.changeFd(PROTECTED_COORD_FD);
  CoordinatorAPI::setCoordPort(coordinatorListenerSocket.port());

  JTRACE("Starting a new coordinator automatically.")
        (coordinatorListenerSocket.port());

  if (fork() == 0) {
    // We can't use Util::getPath() here since the SharedData has not been
    // initialized yet.
    string coordinator =
      jalib::Filesystem::GetProgramDir() + "/dmtcp_coordinator";

    char *modeStr = (char *)"--daemon";
    char * args[] = {
      (char*)coordinator.c_str(),
      (char*)"--quiet",
      /* If we wish to also suppress coordinator warnings, call --quiet twice */
      (char*)"--exit-on-last",
      modeStr,
      NULL
    };
    execv(args[0], args);
    JASSERT(false)(coordinator)(JASSERT_ERRNO) .Text("exec(dmtcp_coordinator) failed");
  } else {
    _real_close (PROTECTED_COORD_FD);
  }

  int status;
  JASSERT(wait(&status) > 0) (JASSERT_ERRNO);
}

void CoordinatorAPI::createNewConnToCoord(CoordinatorMode mode)
{
  if (mode & COORD_JOIN) {
    JTRACE("***COORD_JOIN found!");
    _coordinatorSocket = createNewSocketToCoordinator(mode);
    JASSERT(_coordinatorSocket != -1) (JASSERT_ERRNO)
      .Text("Coordinator not found, but --join was specified. Exiting.");
  } else if (mode & COORD_NEW) {
    JTRACE("***COORD_NEW found!");
    startNewCoordinator(mode);
    _coordinatorSocket = createNewSocketToCoordinator(mode);
    JASSERT(_coordinatorSocket != -1) (JASSERT_ERRNO)
      .Text("Error connecting to newly started coordinator.");
  } else if (mode & COORD_ANY) {
    // _coordinatorSocket = createNewSocketToCoordinator(mode);
    JTRACE("***COORD_ANY found!");
    _coordinatorSocket = createNewSocketToNewCoordinator();

    if (_coordinatorSocket == -1) {
      JTRACE("Coordinator not found, trying to start a new one.");
      startNewCoordinator(mode);
      _coordinatorSocket = createNewSocketToCoordinator(mode);
      JASSERT(_coordinatorSocket != -1) (JASSERT_ERRNO)
        .Text("Error connecting to newly started coordinator.");
    }
  } else {
    JASSERT(false) .Text("Not Reached");
  }
  updateSockFd();
}

DmtcpMessage CoordinatorAPI::sendRecvHandshake(DmtcpMessage msg,
                                               string progname,
                                               UniquePid *compId)
{
  if (dmtcp_virtual_to_real_pid) {
    msg.realPid = dmtcp_virtual_to_real_pid(getpid());
  } else {
    msg.realPid = getpid();
  }

  msg.theCheckpointInterval = getCkptInterval();

  string hostname = jalib::Filesystem::GetCurrentHostname();

  size_t buflen = hostname.length() + progname.length() + 2;
  char buf[buflen];
  strcpy(buf, hostname.c_str());
  strcpy(&buf[hostname.length() + 1], progname.c_str());

  // strcpy(extraName, hostname.c_str());
  // strcpy(&extraName[hostname.length() + 1], progname.c_str());
  extraName = string(buf);

  sendMsgToCoordinator(msg, buf, buflen);

  recvMsgFromCoordinator(&msg);

  JTRACE("Inside sendRecvHandshake checking assertValid ")(msg.type);
  msg.assertValid();
  //* look for new coord make new type here for msg and send from dmtcp coord* //

  if (msg.type == DMT_KILL_PEER) {
    JTRACE("Received KILL message from coordinator, exiting");


    return msg;
    JTRACE("Received KILL message from coordinator, exiting sendRecvHandshake ")(msg.type);

    _real_exit (0);
  }
  if (msg.type == DMT_REJECT_NOT_RUNNING) {
    JASSERT(false)
      .Text("Connection rejected by the coordinator.\n"
            "Reason: Current computation not in RUNNING state.\n"
            "         Is a checkpoint/restart in progress?");
  } else if (msg.type == DMT_REJECT_WRONG_COMP) {
    JASSERT(compId != NULL);
    JASSERT(false) (*compId)
      .Text("Connection rejected by the coordinator.\n"
            " Reason: This process has a different computation group.");
  }
  JASSERT(msg.type == DMT_ACCEPT)(msg.type);
  return msg;
}

/*
void CoordinatorAPI::connectToNewCoordOnStartup1()
{
  JTRACE("connecting to new cooordinator. Oopps!!");
  DmtcpUniqueProcessId compId;
  CoordinatorMode allowedModes = COORD_NEW;
  struct in_addr localIPAddr;
  int altport = 6669;
  CoordinatorInfo *coordInfo;

  // using _cachedHost which is called first time as hostname

  static string thePortFile;

  JTRACE("Getting new cooordinator. maybe Oopps?");

  JTRACE("Creating new Socket to coord with port ");

  // this line is from createNewCoonToCoord
  _coordinatorSocket = jalib::JClientSocket(_cachedHost, altport).sockfd();
  updateSockFd();


  JTRACE("sending coordinator handshake")(UniquePid::ThisProcess());
  DmtcpMessage hello_local(DMT_NEW_WORKER);
  hello_local.virtualPid = -1;

  JTRACE("connectToCoordOnStartup, calling sendRecvHandshake")(extraName);
  DmtcpMessage hello_remote = sendRecvHandshake(hello_local, extraName);
  JTRACE("connectToCoordOnStartup, after sendRecvHandshake");

  JASSERT(hello_remote.virtualPid != -1);
  JTRACE("Got virtual pid from coordinator") (hello_remote.virtualPid);

  pid_t ppid =   getppid();
  Util::setVirtualPidEnvVar(hello_remote.virtualPid, ppid, ppid);

  // JASSERT(compId != NULL && localIPAddr != NULL && coordInfo != NULL);

  // compId = hello_remote.compGroup.upid();
  // coordInfo->id = hello_remote.from.upid();
  // coordInfo->timeStamp = hello_remote.coordTimeStamp;
  // coordInfo->addrLen = sizeof (coordInfo->addr);

 // JASSERT(getpeername(_coordinatorSocket,
 //                     (struct sockaddr*) &coordInfo->addr,
 //                     &coordInfo->addrLen) == 0)
    // (JASSERT_ERRNO);
  // memcpy(localIPAddr, &hello_remote.ipAddr, sizeof hello_remote.ipAddr);
  Util::writeCoordPortToFile(altport, thePortFile.c_str());
}

*/


void leaderFinding(zhandle_t *zh){
  printf("leader finding \n");
    int rc= zoo_get_children(zh, master_znode_base_path, 1, &myvector);
    if(rc != ZOK) {
     fprintf(stderr,"electLeader: get_children failed\n");
    } else {
      int i=0;
      fprintf(stderr, "checking for coordinators which are available \n");
      char other_master_string[11];


      int check_coord_number = -1;
      other_master_string[11] = '\0';

    for(i = 0;i < myvector.count; i++){
          fprintf(stderr, "%s\n", myvector.data[i]);

          strncpy(other_master_string, myvector.data[i] + 12, 10);
          printf("checking other_master node : %s \n", other_master_string);

          // dmtcp_coord_ from dmtcp_coord_0000000001
          check_coord_number = atoi(other_master_string);
          if(master_number > check_coord_number){
            printf("\tupdating master coordinator as %d \n", check_coord_number);
            master_number = check_coord_number;
            strncpy(master_string, myvector.data[i], 23);
          }
          printf("\n");
      }
  }

  fprintf(stderr, "Master determined from znode cluster is  %d with %s \n", master_number, master_string);
  int len = 254;
  struct Stat st;
  char full_master_path[30];
  strcpy(full_master_path, full_master_znode_base_path);

  strcat(full_master_path, master_string);
  // makin NULL instead of watcher_for_wget
  printf("selected path is %s\n",full_master_path);
  rc = zoo_wget(zh, full_master_path, zookeeper::watcher_for_wget, mycontext, buffer, &len, &st);
//  rc = zoo_wget(zh, "/master", zookeeper::watcher_for_wget, mycontext, buffer, &len, &st);
  // rc = zoo_wget_children(zh, full_master_path, zookeeper::watcher_for_wget, mycontext, NULL);
  buffer[strlen(buffer)] = '\0';

  if(rc != ZOK) {
      printf("Problems %s %d\n", full_master_path, rc);
    } else {
      printf("Data: %s\n", buffer);
      int i = 0;
        for(i = 0; buffer[i] != ':'; i++){
            zoohost[i] = buffer[i];
        }
        zoohost[i++] = '\0';
        char portChar[10];
        int j = 0;

        for( i ; i < strlen(buffer) ; i++, j++ ){
          portChar[j] = buffer[i];
        }
        portChar[j] = '\0';

        zooport = atoi(portChar);


        printf("retrieved %s %d\n", zoohost, zooport);
    }
}

void initZooHandle(){

    printf("hello zookeeper\n");

  // char buffer[512];
//  zh = zookeeper_init("localhost:2181", zookeeper::watcher, 30000, 0, 0, 0);
    zh = zookeeper_init("localhost:2181", NULL, 30000, 0, 0, 0);

  if(!zh){
    printf("error");
  }

}

void CoordinatorAPI::getCoordHostAndPortNew(const char **host, int *port)
{
  printf("get new host and port\n");
  if(_firstTimeZoo){
       dmtcp::initZooHandle();
   }
   dmtcp::leaderFinding(zh);
  _cachedHost = zoohost;
  _cachedPort = zooport;
   *host = zoohost;
   *port = zooport;
  _firstTime = false;
  _firstTimeZoo = false;

  return;
}

void CoordinatorAPI::connectToNewCoordOnStartup()
{
  printf("connecting to new cooordinator via zookeeper\n");
  // DmtcpUniqueProcessId compId;
  // CoordinatorMode allowedModes = COORD_NEW;
  // struct in_addr localIPAddr;
  // int altport = 6669;
  // CoordinatorInfo *coordInfo;


  if(_firstTimeZoo){
      printf("**** in  first time zoo\n");
      dmtcp::initZooHandle();
      dmtcp::leaderFinding(zh);
  }
  // using _cachedHost which is called first time as hostname

  static string thePortFile;

  JTRACE("Getting new cooordinator. maybe from zookeeper, Oopps?");
  JTRACE("host from zookeeper")(zoohost);
  JTRACE("port from zookeeper")(zooport);

  JTRACE("Creating new Socket to coord with port ");

  // should i close the connection here?

  // this line is from createNewCoonToCoord
  // _coordinatorSocket = jalib::JClientSocket(_cachedHost, altport).sockfd();
  // make it zookeeper-ish
  _coordinatorSocket = jalib::JClientSocket(zoohost, zooport).sockfd();
  updateSockFd();


  JTRACE("sending coordinator handshake")(UniquePid::ThisProcess());
  DmtcpMessage hello_local(DMT_NEW_WORKER);
  hello_local.virtualPid = -1;

  JTRACE("connectToCoordOnStartup, calling sendRecvHandshake")(extraName);
  DmtcpMessage hello_remote = sendRecvHandshake(hello_local, extraName);
  JTRACE("connectToCoordOnStartup, after sendRecvHandshake");

  JASSERT(hello_remote.virtualPid != -1);
  JTRACE("Got virtual pid from coordinator") (hello_remote.virtualPid);

  pid_t ppid =   getppid();
  Util::setVirtualPidEnvVar(hello_remote.virtualPid, ppid, ppid);

  // JASSERT(compId != NULL && localIPAddr != NULL && coordInfo != NULL);

  // compId = hello_remote.compGroup.upid();
  // coordInfo->id = hello_remote.from.upid();
  // coordInfo->timeStamp = hello_remote.coordTimeStamp;
  // coordInfo->addrLen = sizeof (coordInfo->addr);

/*  JASSERT(getpeername(_coordinatorSocket,
                      (struct sockaddr*) &coordInfo->addr,
                      &coordInfo->addrLen) == 0)*/
    // (JASSERT_ERRNO);
  // memcpy(localIPAddr, &hello_remote.ipAddr, sizeof hello_remote.ipAddr);
  Util::writeCoordPortToFile(zooport, thePortFile.c_str());
}

void CoordinatorAPI::startZookeeperinstance(){
  if(_firstTimeZoo){
    printf("*** Method : startZookeeperinstance :::First time zoo\n");
    dmtcp::initZooHandle();
    dmtcp::leaderFinding(zh);
  }else{
    printf("*** Method : startZookeeperinstance :::not first time zoo\n");
  }
}

void CoordinatorAPI::connectToCoordOnStartup(CoordinatorMode mode,
                                             string progname,
                                             DmtcpUniqueProcessId *compId,
                                             CoordinatorInfo *coordInfo,
                                             struct in_addr  *localIP)
{
  JASSERT(compId != NULL && localIP != NULL && coordInfo != NULL);

  if (mode & COORD_NONE) {
    setupVirtualCoordinator(coordInfo, localIP);
    *compId = coordInfo->id;
    return;
  }

  createNewConnToCoord(mode);
  JTRACE("sending coordinator handshake")(UniquePid::ThisProcess());
  DmtcpMessage hello_local(DMT_NEW_WORKER);
  hello_local.virtualPid = -1;

  JTRACE("connectToCoordOnStartup, calling sendRecvHandshake")(progname);
    if(_firstTimeZoo){
      printf("************************AGAIN STAR!! in  first time zoo\n");
      dmtcp::initZooHandle();
      dmtcp::leaderFinding(zh);
  }else{
      printf("************************AGAIN STAR!! is not first time \n");
  }
  DmtcpMessage hello_remote = sendRecvHandshake(hello_local, progname);
  JTRACE("connectToCoordOnStartup, after sendRecvHandshake");

  JASSERT(hello_remote.virtualPid != -1);
  JTRACE("Got virtual pid from coordinator") (hello_remote.virtualPid);

  pid_t ppid = getppid();
  Util::setVirtualPidEnvVar(hello_remote.virtualPid, ppid, ppid);

  JASSERT(compId != NULL && localIP != NULL && coordInfo != NULL);
  *compId = hello_remote.compGroup.upid();
  coordInfo->id = hello_remote.from.upid();
  coordInfo->timeStamp = hello_remote.coordTimeStamp;
  coordInfo->addrLen = sizeof (coordInfo->addr);
  JASSERT(getpeername(_coordinatorSocket,
                      (struct sockaddr*) &coordInfo->addr,
                      &coordInfo->addrLen) == 0)
    (JASSERT_ERRNO);
  memcpy(localIP, &hello_remote.ipAddr, sizeof hello_remote.ipAddr);

}

void CoordinatorAPI::createNewConnectionBeforeFork(string& progname)
{
  JASSERT(!noCoordinator())
    .Text("Process attempted to call fork() while in --no-coordinator mode\n"
          "  Because the coordinator is embedded in a single process,\n"
          "    DMTCP will not work with multiple processes.");
  struct sockaddr_storage addr;
  uint32_t len;
  SharedData::getCoordAddr((struct sockaddr *)&addr, &len);
  socklen_t addrlen = len;
  _coordinatorSocket = jalib::JClientSocket((struct sockaddr *)&addr, addrlen);
  JASSERT(_coordinatorSocket != -1);

  DmtcpMessage hello_local(DMT_NEW_WORKER);
  DmtcpMessage hello_remote = sendRecvHandshake(hello_local, progname);
  JASSERT(hello_remote.virtualPid != -1);

  if (dmtcp_virtual_to_real_pid) {
    JTRACE("Got virtual pid from coordinator") (hello_remote.virtualPid);
    pid_t pid = getpid();
    pid_t realPid = dmtcp_virtual_to_real_pid(pid);
    Util::setVirtualPidEnvVar(hello_remote.virtualPid, pid, realPid);
  }
}

void CoordinatorAPI::connectToCoordOnRestart(CoordinatorMode  mode,
                                             string progname,
                                             UniquePid compGroup,
                                             int np,
                                             CoordinatorInfo *coordInfo,
                                             const char *host,
                                             int port,
                                             struct in_addr  *localIP)
{
  if (mode & COORD_NONE) {
    setupVirtualCoordinator(coordInfo, localIP);
    return;
  }

  createNewConnToCoord(mode);
  JTRACE("sending coordinator handshake")(UniquePid::ThisProcess());
  DmtcpMessage hello_local(DMT_RESTART_WORKER);
  hello_local.virtualPid = -1;
  hello_local.numPeers = np;
  hello_local.compGroup = compGroup;

  DmtcpMessage hello_remote = sendRecvHandshake(hello_local, progname,
                                                &compGroup);

  if (coordInfo != NULL) {
    coordInfo->id = hello_remote.from.upid();
    coordInfo->timeStamp = hello_remote.coordTimeStamp;
    coordInfo->addrLen = sizeof (coordInfo->addr);
    JASSERT(getpeername(_coordinatorSocket,
                        (struct sockaddr*) &coordInfo->addr,
                        &coordInfo->addrLen) == 0)
      (JASSERT_ERRNO);
  }
  if (localIP != NULL) {
    memcpy(localIP, &hello_remote.ipAddr, sizeof hello_remote.ipAddr);
  }

  JTRACE("Coordinator handshake RECEIVED!!!!!");
}

void CoordinatorAPI::sendCkptFilename()
{
  if (noCoordinator()) return;
  // Tell coordinator to record our filename in the restart script
  string ckptFilename = ProcessInfo::instance().getCkptFilename();
  string hostname = jalib::Filesystem::GetCurrentHostname();
  JTRACE("recording filenames") (ckptFilename) (hostname);
  DmtcpMessage msg;
  if (dmtcp_unique_ckpt_enabled && dmtcp_unique_ckpt_enabled()) {
    msg.type = DMT_UNIQUE_CKPT_FILENAME;
  } else {
    msg.type = DMT_CKPT_FILENAME;
  }

  size_t buflen = hostname.length() + ckptFilename.length() + 2;
  char buf[buflen];
  strcpy(buf, ckptFilename.c_str());
  strcpy(&buf[ckptFilename.length() + 1], hostname.c_str());

  sendMsgToCoordinator(msg, buf, buflen);
}


int CoordinatorAPI::sendKeyValPairToCoordinator(const char *id,
                                                const void *key,
                                                uint32_t key_len,
                                                const void *val,
                                                uint32_t val_len,
                                                int sync)
{
  DmtcpMessage msg (DMT_REGISTER_NAME_SERVICE_DATA);
  if (sync) {
    msg.type = DMT_REGISTER_NAME_SERVICE_DATA_SYNC;
  }
  JWARNING(strlen(id) < sizeof(msg.nsid));
  strncpy(msg.nsid, id, 8);
  msg.keyLen = key_len;
  msg.valLen = val_len;
  msg.extraBytes = key_len + val_len;
  int sock = _coordinatorSocket;
  if (dmtcp_is_running_state()) {
    if (_nsSock == -1) {
      _nsSock = createNewSocketToCoordinator(COORD_ANY);
      JASSERT(_nsSock != -1);
      _nsSock = Util::changeFd(_nsSock, PROTECTED_NS_FD);
      sock = _nsSock;
      DmtcpMessage m(DMT_NAME_SERVICE_WORKER);
      JASSERT(Util::writeAll(sock, &m, sizeof(m)) == sizeof(m));
    }
    sock = _nsSock;
  }

  JASSERT(Util::writeAll(sock, &msg, sizeof(msg)) == sizeof(msg));
  JASSERT(Util::writeAll(sock, key, key_len) == key_len);
  JASSERT(Util::writeAll(sock, val, val_len) == val_len);

  if (sync) {
    msg.poison();
    JASSERT(Util::readAll(sock, &msg, sizeof(msg)) == sizeof(msg));
    msg.assertValid();
    JASSERT(msg.type == DMT_REGISTER_NAME_SERVICE_DATA_SYNC_RESPONSE)(msg.type);
  }
  return 1;
}

// On input, val points to a buffer in user memory and *val_len is the maximum
//   size of that buffer (the memory allocated by user).
// On output, we copy data to val, and set *val_len to the actual buffer size
//   (to the size of the data that we copied to the user buffer).
int CoordinatorAPI::sendQueryToCoordinator(const char *id,
                                           const void *key,
                                           uint32_t key_len,
                                           void *val,
                                           uint32_t *val_len)
{
  DmtcpMessage msg (DMT_NAME_SERVICE_QUERY);
  JWARNING(strlen(id) < sizeof(msg.nsid));
  strncpy(msg.nsid, id, 8);
  msg.keyLen = key_len;
  msg.valLen = 0;
  msg.extraBytes = key_len;
  int sock = _coordinatorSocket;

  if (key == NULL || key_len == 0 || val == NULL || val_len == 0) {
    return 0;
  }

  if (dmtcp_is_running_state()) {
    if (_nsSock == -1) {
      _nsSock = createNewSocketToCoordinator(COORD_ANY);
      JASSERT(_nsSock != -1);
      _nsSock = Util::changeFd(_nsSock, PROTECTED_NS_FD);
      JASSERT(_nsSock == PROTECTED_NS_FD);
      DmtcpMessage m(DMT_NAME_SERVICE_WORKER);
      JASSERT(Util::writeAll(_nsSock, &m, sizeof(m)) == sizeof(m));
    }
    sock = _nsSock;
  }

  JASSERT(Util::writeAll(sock, &msg, sizeof(msg)) == sizeof(msg));
  JASSERT(Util::writeAll(sock, key, key_len) == key_len);

  msg.poison();

  JASSERT(Util::readAll(sock, &msg, sizeof(msg)) == sizeof(msg));
  msg.assertValid();
  JASSERT(msg.type == DMT_NAME_SERVICE_QUERY_RESPONSE &&
          msg.extraBytes == msg.valLen);

  JASSERT (*val_len >= msg.valLen);
  *val_len = msg.valLen;
  JASSERT(Util::readAll(sock, val, *val_len) == *val_len);

  return *val_len;
}

/*
 * Setup a virtual coordinator. It's part of the running process (i.e., no
 * separate process is created).
 *
 * FIXME: This is the only place in this file where we use JSocket. May be get
 * rid of it here too?
 */
void CoordinatorAPI::setupVirtualCoordinator(CoordinatorInfo *coordInfo,
                                             struct in_addr  *localIP)
{
  const char *host = NULL;
  int port;
  CoordinatorAPI::getCoordHostAndPort(COORD_NONE, &host, &port);
  jalib::JSocket sock =
    jalib::JServerSocket(jalib::JSockAddr::ANY, port).sockfd();
  _coordinatorSocket = sock.sockfd();
  JASSERT(_coordinatorSocket != -1) (port) (JASSERT_ERRNO)
    .Text("Failed to create listen socket.");
  updateSockFd();
  CoordinatorAPI::setCoordPort(sock.port());

  pid_t ppid = getppid();
  Util::setVirtualPidEnvVar(INITIAL_VIRTUAL_PID, ppid, ppid);

  UniquePid coordId = UniquePid(INITIAL_VIRTUAL_PID,
                                UniquePid::ThisProcess().hostid(),
                                UniquePid::ThisProcess().time());

  coordInfo->id = coordId.upid();
  coordInfo->timeStamp = coordId.time();
  coordInfo->addrLen = 0;
  if (getenv(ENV_VAR_CKPT_INTR) != NULL) {
    coordInfo->interval = (uint32_t) strtol(getenv(ENV_VAR_CKPT_INTR), NULL, 0);
  } else {
    coordInfo->interval = 0;
  }
  memset(&coordInfo->addr, 0, sizeof(coordInfo->addr));
  memset(localIP, 0, sizeof(*localIP));
}

void CoordinatorAPI::waitForCheckpointCommand()
{
  uint32_t ckptInterval = SharedData::getCkptInterval();
  struct timeval tmptime={0,0};
  long remaining = ckptInterval;
  do {
    fd_set rfds;
    struct timeval *timeout = NULL;
    struct timeval start;
    if (ckptInterval > 0) {
      timeout = &tmptime;
      timeout->tv_sec = remaining;
      JASSERT(gettimeofday(&start, NULL) == 0) (JASSERT_ERRNO);
    }

    // This call to select() does nothing and returns.
    // But we want to find address of select() using dlsym/libc before
    //   allowing the user thread to continue.
    struct timeval timezero = {0,0};
    select(0, NULL, NULL, NULL, &timezero);
    if (sem_launch_first_time) {
      // Release user thread now that we've initialized the checkpoint thread.
      // This code is reached if the --no-coordinator flag is used.
      sem_post(&sem_launch);
      sem_launch_first_time = false;
    }

    FD_ZERO(&rfds);
    FD_SET(_coordinatorSocket, &rfds );
    int retval =
      select(_coordinatorSocket + 1, &rfds, NULL, NULL, timeout);
    if (retval == 0) { // timeout expired, time for checkpoint
      JTRACE("Timeout expired, checkpointing now.");
      return;
    } else if (retval > 0) {
      JASSERT(FD_ISSET(_coordinatorSocket, &rfds));
      JTRACE("Connect request on virtual coordinator socket.");
      break;
    }
    JASSERT(errno == EINTR) (JASSERT_ERRNO); /* EINTR: a signal was caught */
    if (ckptInterval > 0) {
      struct timeval end;
      JASSERT(gettimeofday(&end, NULL) == 0) (JASSERT_ERRNO);
      remaining -= end.tv_sec - start.tv_sec;
      // If the remaining time is negative, we can checkpoint now
      if (remaining < 0) {
        return;
      }
    }
  } while (remaining > 0);

  jalib::JSocket cmdSock(-1);
  DmtcpMessage msg;
  DmtcpMessage reply(DMT_USER_CMD_RESULT);
  do {
    cmdSock.close();
    jalib::JServerSocket sock(_coordinatorSocket);
    cmdSock = sock.accept();
    msg.poison();
    JTRACE("Reading from incoming connection...");
    cmdSock >> msg;
  } while (!cmdSock.isValid());

  JASSERT(msg.type == DMT_USER_CMD) (msg.type)
    .Text("Unexpected connection.");

  reply.coordCmdStatus = CoordCmdStatus::NOERROR;

  bool exitWhenDone = false;
  switch (msg.coordCmd) {
//    case 'b': case 'B':  // prefix blocking command, prior to checkpoint command
//      JTRACE("blocking checkpoint beginning...");
//      blockUntilDone = true;
//      break;
    case 's': case 'S':
      JTRACE("Received status command");
      reply.numPeers = 1;
      reply.isRunning = 1;
      break;
    case 'c': case 'C':
      JTRACE("checkpointing...");
      break;
    case 'k': case 'K':
    case 'q': case 'Q':
      JTRACE("Received KILL command from user, exiting");
      exitWhenDone = true;
      break;
    default:
      JTRACE("unhandled user command") (msg.coordCmd);
      reply.coordCmdStatus = CoordCmdStatus::ERROR_INVALID_COMMAND;
  }
  cmdSock << reply;
  cmdSock.close();
  if (exitWhenDone) {
    JASSERT_STDERR << "KILL from coord waitForCheckpointCommand \n";
    _real_exit(0);
  }
  return;
}

bool CoordinatorAPI::noCoordinator()
{
  static int virtualCoordinator = -1;
  if (virtualCoordinator == -1) {
    int optVal = -1;
    socklen_t optLen = sizeof(optVal);
    int ret = _real_getsockopt(PROTECTED_COORD_FD, SOL_SOCKET,
                               SO_ACCEPTCONN, &optVal, &optLen);
    if (ret == 0 && optVal == 1) {
      virtualCoordinator = 1;
    } else {
      virtualCoordinator = 0;
    }
  }
  return virtualCoordinator;
}

}

namespace zookeeper{
  #include <zookeeper/zookeeper.h>
void watcher_for_wget(zhandle_t *zh, int type, int state, const char *path,
             void* context)
{
    printf("*** watcher_for_wget event called %d for state %d path %s \n", type,state, path);
    dmtcp::_firstTimeZoo = false;
    char buffer[512];
    int len, rc;
    struct Stat st;
    char *p = (char *)context;
    if (type == ZOO_SESSION_EVENT) {
        if (state == ZOO_CONNECTED_STATE) {
          printf("in watcher state == connected state \n");
            return;
        } else if (state == ZOO_AUTH_FAILED_STATE) {
          printf("in watcher state == failed \n");
            zookeeper_close(dmtcp::zh);
            exit(1);
        } else if (state == ZOO_EXPIRED_SESSION_STATE) {
          printf("in watcher state ==expired \n");
            dmtcp::master_number = 999;
             dmtcp::initZooHandle();
            dmtcp::leaderFinding(dmtcp::zh);
        }
    } else if (type == ZOO_CHANGED_EVENT) {
        printf("Data changed for %s \n", path);
        len = 254;
        //get the changed data and set an watch again
        rc = zoo_wget(zh, path, watcher_for_wget , dmtcp::mycontext, buffer, &len, &st);
        if (ZOK != rc){
            printf("Problems %s %d\n", path, rc);
        } else if (len >= 0) {
           buffer[len] = 0;
           printf("Path: %s changed data: %s\n", path, buffer);
        }
    } else if (type == ZOO_DELETED_EVENT ){
      printf("some node was deleted, lets elect leader again! \n");
      // call leader election here
      // before election remove the cached master with us
      dmtcp::master_number = 999;
      dmtcp::leaderFinding(dmtcp::zh);
      dmtcp::CoordinatorAPI::instance().connectToNewCoordOnStartup();
    }

    printf("Watcher call completed,  context: %s\n", p);
}

void watcher(zhandle_t *zh, int type, int state, const char *path,
             void* context)
{
    if (type == ZOO_SESSION_EVENT) {
      printf("************** zoo event changed : watcher \n");
        if (state == ZOO_CONNECTED_STATE) {
              printf("watcher: state == connected\n");
        } else if (state == ZOO_AUTH_FAILED_STATE) {
          printf("watcher: state == auth failed\n");
            zookeeper_close(zh);
            exit(1);
        } else if (state == ZOO_EXPIRED_SESSION_STATE) {
          printf("watcher: state == expired session state\n");
            zookeeper_close(zh);
            exit(1);
        }

    }
    printf("init watcher called! \n");
}
}
