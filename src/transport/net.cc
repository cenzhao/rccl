/*************************************************************************
 * Copyright (c) 2016-2022, NVIDIA CORPORATION. All rights reserved.
 * Modifications Copyright (c) 2019-2022 Advanced Micro Devices, Inc. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#include "comm.h"
#include "net.h"
#include "graph.h"
#include <sys/time.h>
#include "proxy.h"
#include "collectives.h"
#include "gdrwrap.h"
#include "shm.h"
#include "profiler.h"

static_assert(sizeof(ncclNetHandle_t) <= CONNECT_SIZE, "NET Connect info is too large");

#define NCCL_NET_MAP_HOSTMEM 0
#define NCCL_NET_MAP_DEVMEM 1
#define NCCL_NET_MAP_SHARED_HOSTMEM 2
#define NCCL_NET_MAP_SHARED_DEVMEM 3
#define NCCL_NET_MAP_GDCMEM 4
#define NCCL_NET_MAP_MEMS 5

#define NCCL_NET_MAP_MASK_DEVMEM 0x40000000
#define NCCL_NET_MAP_MASK_SHARED 0x80000000
#define NCCL_NET_MAP_MASK_USED   0x20000000
#define NCCL_NET_MAP_MASK_OFFSET 0x1fffffff

#define NCCL_NET_MAP_OFFSET_BANK(mapStruct, offsetName) \
  ((mapStruct)->offsets.offsetName >> 30)

#define NCCL_NET_MAP_OFFSET_NULL(mapStruct, offsetName) \
  (((mapStruct)->offsets.offsetName >> 29) == 0)

#define NCCL_NET_MAP_GET_POINTER(mapStruct, cpuOrGpu, offsetName) \
  (NCCL_NET_MAP_OFFSET_NULL(mapStruct, offsetName) ? NULL : \
   (mapStruct)->mems[NCCL_NET_MAP_OFFSET_BANK(mapStruct, offsetName)].cpuOrGpu##Ptr + ((mapStruct)->offsets.offsetName & NCCL_NET_MAP_MASK_OFFSET))

#define NCCL_NET_MAP_DEV_MEM(mapStruct, offsetName) \
  (((mapStruct)->offsets.offsetName & NCCL_NET_MAP_MASK_DEVMEM) != 0)

#define NCCL_NET_MAP_ADD_POINTER(mapStruct, shared, dev, memSize, offsetName) do { \
    int bank = NCCL_NET_MAP_MASK_USED + (dev)*NCCL_NET_MAP_MASK_DEVMEM + (shared)*NCCL_NET_MAP_MASK_SHARED; \
    if ((shared) == 0) { \
      if (dev) { \
        (mapStruct)->offsets.offsetName = bank + (mapStruct)->mems[NCCL_NET_MAP_DEVMEM].size; \
        (mapStruct)->mems[NCCL_NET_MAP_DEVMEM].size += memSize; \
      } else { \
        (mapStruct)->offsets.offsetName = bank + (mapStruct)->mems[NCCL_NET_MAP_HOSTMEM].size; \
        (mapStruct)->mems[NCCL_NET_MAP_HOSTMEM].size += memSize; \
      } \
    } else { \
      (mapStruct)->offsets.offsetName = bank; \
    } \
} while (0);

struct connectMapMem{
  char* gpuPtr;
  char* cpuPtr;
  int size;
  union {
    char shmPath[PATH_MAX];
    hipIpcMemHandle_t ipc;
  };
};

struct connectMap {
  int sameProcess;
  int shared;
  int cudaDev;
  // First 3 bits of offsets determine the mem bank. 001 is host mem, 011 is dev mem, 101 is shared host mem and 111 is shared dev mem.
  struct connectMapMem mems[NCCL_NET_MAP_MEMS];
  // Offsets. 3 MSBs indicate mem bank, 111 indicates NULL.
  struct {
    uint32_t sendMem;
    uint32_t recvMem;
    uint32_t buffs[NCCL_NUM_PROTOCOLS];
  } offsets;
};

struct sendResources {
  struct connectMap map;
  void* netSendComm;
  struct ncclSendMem* sendMem;
  struct ncclRecvMem* recvMem;

  int rank;
  int localRank;
  int remoteRank;
  int netDev;
  int useGdr;
  int maxRecvs;
  uint64_t* gdcSync;
  void* gdrDesc;
  int shared;
  int channelId;
  int connIndex;
  char* buffers[NCCL_NUM_PROTOCOLS];
  int buffSizes[NCCL_NUM_PROTOCOLS];
  void* mhandles[NCCL_NUM_PROTOCOLS];
  uint64_t step;
  uint64_t llLastCleaning;
  uint32_t* curr_hdp_reg;  // Curr GPU in ring (for rdma transport use only)
};

struct recvResources {
  struct connectMap map;
  void* netListenComm;
  void* netRecvComm;
  struct ncclSendMem* sendMem;
  struct ncclRecvMem* recvMem;

  int rank;
  int localRank;
  int remoteRank;
  int proxyRank;
  int netDev;
  int useGdr;
  int maxRecvs;
  uint64_t* gdcSync;
  uint64_t* gdcFlush;
  void* gdrDesc;
  int shared;
  int channelId;
  int connIndex;
  char* buffers[NCCL_NUM_PROTOCOLS];
  int buffSizes[NCCL_NUM_PROTOCOLS];
  void* mhandles[NCCL_NUM_PROTOCOLS];
  uint64_t step;
  uint64_t llLastCleaning;
  uint32_t* curr_hdp_reg;  // Curr GPU in ring (for rdma transport use only)
};

NCCL_PARAM(NetDisableIntra, "NET_DISABLE_INTRA", -2);

/* Determine if two peers can communicate with NET */
static ncclResult_t canConnect(int* ret, struct ncclTopoSystem* topo, struct ncclTopoGraph* graph, struct ncclPeerInfo* info1, struct ncclPeerInfo* info2) {
  // Same host?
  if (info1->hostHash == info2->hostHash) {
    // User disabled NET for intra-node?
    if (ncclParamNetDisableIntra() == 1) {
      *ret = 0;
      return ncclSuccess;
    }
  }
  *ret = 1;
  return ncclSuccess;
}

NCCL_PARAM(NetSharedBuffers, "NET_SHARED_BUFFERS", -2);
NCCL_PARAM(NetSharedComms, "NET_SHARED_COMMS", 1);

struct setupReq {
  int rank;
  int localRank;
  int remoteRank;
  int shared;
  int netDev;
  int useGdr;
  int channelId;
  int connIndex;
};

/* Determine if we will use this transport for this peer and return connect
 * information for this peer */
static ncclResult_t sendSetup(struct ncclComm* comm, struct ncclTopoGraph* graph, struct ncclPeerInfo* myInfo, struct ncclPeerInfo* peerInfo, struct ncclConnect* connectInfo, struct ncclConnector* send, int channelId, int connIndex) {
  struct setupReq req;

  send->conn.shared = req.shared = graph ? 0 : ncclParamNetSharedBuffers() != -2 ? ncclParamNetSharedBuffers() : 1;
  req.channelId = channelId;
  req.connIndex = connIndex;
  req.netDev = -1;

  int proxyRank = myInfo->rank;
  if (connIndex == NCCL_CONN_IDX_P2P_NET) NCCLCHECK(ncclTopoGetIntraNetDev(comm->topo, myInfo->rank, graph, channelId, 1, &req.netDev));
  if (req.netDev < 0) NCCLCHECK(ncclTopoGetNetDev(comm, myInfo->rank, graph, channelId, peerInfo->rank, &req.netDev, &proxyRank));
  NCCLCHECK(ncclTopoCheckGdr(comm->topo, myInfo->busId, req.netDev, 1, &req.useGdr));
  send->conn.direct |= req.useGdr ? NCCL_DIRECT_NIC : 0;

  NCCLCHECK(ncclProxyConnect(comm, TRANSPORT_NET, 1, proxyRank, &send->proxyConn));
  req.rank = myInfo->rank;
  NCCLCHECK(ncclTopoGetLocalRank(comm->topo, myInfo->rank, &req.localRank));
  req.remoteRank = peerInfo->rank;
  NCCLCHECK(ncclProxyCall(&send->proxyConn, ncclProxyMsgSetup, &req, sizeof(req), NULL, 0));

  if (proxyRank == myInfo->rank) {
    INFO(NCCL_INIT|NCCL_NET,"Channel %02d/%d : %d[%lx] -> %d[%lx] [send] via NET/%s/%d%s%s", channelId, connIndex, myInfo->rank, myInfo->busId, peerInfo->rank, peerInfo->busId, ncclNetName(), req.netDev,
        req.useGdr ? "/GDRDMA" : "", req.shared ? "/Shared" : "");
  } else {
    INFO(NCCL_INIT|NCCL_NET,"Channel %02d/%d : %d[%lx] -> %d[%lx] [send] via NET/%s/%d(%d)%s%s", channelId, connIndex, myInfo->rank, myInfo->busId, peerInfo->rank, peerInfo->busId, ncclNetName(), req.netDev,
        proxyRank, req.useGdr ? "/GDRDMA" : "", req.shared ? "/Shared" : "");
  }
  *((int*)connectInfo) = proxyRank;
  return ncclSuccess;
}

// GDRCOPY support: TAIL_ENABLE When enabled locates the RX proxy tail in CUDA memory
NCCL_PARAM(GdrCopySyncEnable, "GDRCOPY_SYNC_ENABLE", 1);
// GDRCOPY support: FLUSH_ENABLE When enabled uses a PCI-E read to flush GDRDMA buffers
NCCL_PARAM(GdrCopyFlushEnable, "GDRCOPY_FLUSH_ENABLE", 0);

/* Setup recv connector */
static ncclResult_t recvSetup(struct ncclComm* comm, struct ncclTopoGraph* graph, struct ncclPeerInfo* myInfo, struct ncclPeerInfo* peerInfo, struct ncclConnect* connectInfo, struct ncclConnector* recv, int channelId, int connIndex) {
  struct setupReq req;

  recv->conn.shared = req.shared = graph ? 0 : ncclParamNetSharedBuffers() != -2 ? ncclParamNetSharedBuffers() : 1;
  req.channelId = channelId;
  req.connIndex = connIndex;
  req.netDev = -1;

  // Use myInfo->rank as the receiver uses its own NIC
  int proxyRank = myInfo->rank;
  if (connIndex == NCCL_CONN_IDX_P2P_NET) NCCLCHECK(ncclTopoGetIntraNetDev(comm->topo, myInfo->rank, graph, channelId, 0, &req.netDev));
  if (req.netDev < 0) NCCLCHECK(ncclTopoGetNetDev(comm, myInfo->rank, graph, channelId, myInfo->rank, &req.netDev, &proxyRank));
  NCCLCHECK(ncclTopoCheckGdr(comm->topo, myInfo->busId, req.netDev, 0, &req.useGdr));

  // We don't support PXN on receive yet
  NCCLCHECK(ncclProxyConnect(comm, TRANSPORT_NET, 0, myInfo->rank, &recv->proxyConn));

  req.rank = myInfo->rank;
  NCCLCHECK(ncclTopoGetLocalRank(comm->topo, myInfo->rank, &req.localRank));
  req.remoteRank = peerInfo->rank;
  NCCLCHECK(ncclProxyCall(&recv->proxyConn, ncclProxyMsgSetup, &req, sizeof(req), connectInfo, sizeof(ncclNetHandle_t)));

  INFO(NCCL_INIT|NCCL_NET,"Channel %02d/%d : %d[%lx] -> %d[%lx] [receive] via NET/%s/%d%s%s", channelId, connIndex, peerInfo->rank, peerInfo->busId, myInfo->rank, myInfo->busId, ncclNetName(), req.netDev,
      req.useGdr ? "/GDRDMA" : "", req.shared ? "/Shared" : "");
  return ncclSuccess;
}

static ncclResult_t netMapShm(struct connectMapMem* mem) {
  NCCLCHECK(ncclShmOpen(mem->shmPath, mem->size, (void**)&mem->cpuPtr, (void**)&mem->gpuPtr, 0));
  NCCLCHECK(ncclShmUnlink(mem->shmPath));
  return ncclSuccess;
}
static ncclResult_t netCreateShm(struct connectMapMem* mem) {
  mem->shmPath[0] = '\0'; // Let ncclShmOpen create a tmp file
  NCCLCHECK(ncclShmOpen(mem->shmPath, mem->size, (void**)&mem->cpuPtr, NULL, 1));
  return ncclSuccess;
}

static ncclResult_t netDumpMap(struct connectMap* map) {
  printf("Dump map same process %d shared %d\n", map->sameProcess, map->shared);
  struct connectMapMem *mem = map->mems+NCCL_NET_MAP_HOSTMEM;
  printf("Mem 0: Host mem %s (%x B) CPU %p GPU %p\n", mem->shmPath, mem->size, mem->cpuPtr, mem->gpuPtr);
  mem = map->mems+NCCL_NET_MAP_DEVMEM;
  printf("Mem 1: Vid  mem (%x B) CPU %p GPU %p\n", mem->size, mem->cpuPtr, mem->gpuPtr);
  mem = map->mems+NCCL_NET_MAP_SHARED_HOSTMEM;
  printf("Mem 2: Shared Host mem %s (%x B) CPU %p GPU %p\n", mem->shmPath, mem->size, mem->cpuPtr, mem->gpuPtr);
  mem = map->mems+NCCL_NET_MAP_SHARED_DEVMEM;
  printf("Mem 3: Shared Vid mem (%x B) CPU %p GPU %p\n", mem->size, mem->cpuPtr, mem->gpuPtr);
  printf("SendMem -> Used %d Bank %d Offset %x, cpu %p gpu %p\n",
      map->offsets.sendMem & NCCL_NET_MAP_MASK_USED ? 1 : 0,
      NCCL_NET_MAP_OFFSET_BANK(map, sendMem), map->offsets.sendMem & NCCL_NET_MAP_MASK_OFFSET,
      NCCL_NET_MAP_GET_POINTER(map, cpu, sendMem), NCCL_NET_MAP_GET_POINTER(map, gpu, sendMem));
  printf("RecvMem -> Used %d Bank %d Offset %x, cpu %p gpu %p\n",
      map->offsets.recvMem & NCCL_NET_MAP_MASK_USED ? 1 : 0,
      NCCL_NET_MAP_OFFSET_BANK(map, recvMem), map->offsets.recvMem & NCCL_NET_MAP_MASK_OFFSET,
      NCCL_NET_MAP_GET_POINTER(map, cpu, recvMem), NCCL_NET_MAP_GET_POINTER(map, gpu, recvMem));
  for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
    printf("Proto %d -> Used %d Bank %d Offset %x, cpu %p, gpu %p\n", p,
        map->offsets.buffs[p] & NCCL_NET_MAP_MASK_USED ? 1 : 0,
        NCCL_NET_MAP_OFFSET_BANK(map, buffs[p]), map->offsets.buffs[p] & NCCL_NET_MAP_MASK_OFFSET,
        NCCL_NET_MAP_GET_POINTER(map, cpu, buffs[p]), NCCL_NET_MAP_GET_POINTER(map, gpu, buffs[p]));
  }
  printf("End of dump\n");
  return ncclSuccess;
}

static ncclResult_t sendConnect(struct ncclComm* comm, struct ncclConnect* connectInfo, int nranks, int rank, struct ncclConnector* send) {
  // Setup device pointers
  struct connectMap* map;
  NCCLCHECK(ncclCalloc(&map, 1));
  send->transportResources = map;
  NCCLCHECK(ncclProxyCall(&send->proxyConn, ncclProxyMsgConnect, connectInfo, sizeof(ncclNetHandle_t), map, sizeof(struct connectMap)));

  if (map->sameProcess) {
    if (map->cudaDev != comm->cudaDev) {
      // Enable P2P access
      hipError_t err = hipDeviceEnablePeerAccess(map->cudaDev, 0);
      if (err == hipErrorPeerAccessAlreadyEnabled) {
        hipGetLastError();
      } else if (err != hipSuccess) {
        WARN("failed to peer with device %d: %d %s", map->cudaDev, err, hipGetErrorString(err));
        return ncclInternalError;
      }
    }
  } else {
    NCCLCHECK(netMapShm(map->mems+NCCL_NET_MAP_HOSTMEM));
    if (map->mems[NCCL_NET_MAP_DEVMEM].size) {
      CUDACHECK(hipIpcOpenMemHandle((void**)&map->mems[NCCL_NET_MAP_DEVMEM].gpuPtr, map->mems[NCCL_NET_MAP_DEVMEM].ipc, hipIpcMemLazyEnablePeerAccess));
      map->mems[NCCL_NET_MAP_DEVMEM].cpuPtr = NULL;
    }
    if (map->mems[NCCL_NET_MAP_SHARED_DEVMEM].size) {
      void** sharedDevMemPtr = comm->proxyState.sharedDevMems+send->proxyConn.localRank;
      if (*sharedDevMemPtr == NULL) {
        CUDACHECK(hipIpcOpenMemHandle(sharedDevMemPtr, map->mems[NCCL_NET_MAP_SHARED_DEVMEM].ipc, hipIpcMemLazyEnablePeerAccess));
      }
      map->mems[NCCL_NET_MAP_SHARED_DEVMEM].gpuPtr = (char*)(*sharedDevMemPtr);
      map->mems[NCCL_NET_MAP_SHARED_DEVMEM].cpuPtr = NULL;
    }
  }
  //NCCLCHECK(netDumpMap(map));

  struct ncclSendMem *sendMem = (struct ncclSendMem*) NCCL_NET_MAP_GET_POINTER(map, gpu, sendMem);
  void* gdcMem = map->mems[NCCL_NET_MAP_GDCMEM].gpuPtr;
  send->conn.head = gdcMem ? (uint64_t*)gdcMem : &sendMem->head;

  struct ncclRecvMem *recvMem = (struct ncclRecvMem*) NCCL_NET_MAP_GET_POINTER(map, gpu, recvMem);
  send->conn.tail = &recvMem->tail;
  send->conn.sizesFifo = recvMem->sizesFifo;
  // Only fuse P2P buffers, continue to allocate dedicated buffers for ring/tree
  send->conn.offsFifo = map->shared ? recvMem->offsFifo : NULL;

  for (int p=0; p<NCCL_NUM_PROTOCOLS; p++)
    send->conn.buffs[p] = NCCL_NET_MAP_GET_POINTER(map, gpu, buffs[p]);
  return ncclSuccess;
}

/* Connect to this peer */
static ncclResult_t recvConnect(struct ncclComm* comm, struct ncclConnect* connectInfo, int nranks, int rank, struct ncclConnector* recv) {
  struct connectMap* map;
  NCCLCHECK(ncclCalloc(&map, 1));
  recv->transportResources = map;
  NCCLCHECK(ncclProxyCall(&recv->proxyConn, ncclProxyMsgConnect, connectInfo, sizeof(int), map, sizeof(struct connectMap)));
  //NCCLCHECK(netDumpMap(map));

  struct ncclSendMem *sendMem = (struct ncclSendMem*) NCCL_NET_MAP_GET_POINTER(map, gpu, sendMem);
  recv->conn.head = &sendMem->head;

  struct ncclRecvMem *recvMem = (struct ncclRecvMem*) NCCL_NET_MAP_GET_POINTER(map, gpu, recvMem);
  void* gdcMem = map->mems[NCCL_NET_MAP_GDCMEM].gpuPtr;
  recv->conn.tail = gdcMem ? (uint64_t*)gdcMem : &recvMem->tail;
  recv->conn.sizesFifo = recvMem->sizesFifo;
  // Only fuse P2P buffers, continue to allocate dedicated buffers for ring/tree
  recv->conn.offsFifo = map->shared ? recvMem->offsFifo : NULL;

  for (int p=0; p<NCCL_NUM_PROTOCOLS; p++)
    recv->conn.buffs[p] = NCCL_NET_MAP_GET_POINTER(map, gpu, buffs[p]);
  return ncclSuccess;
}

static ncclResult_t sendFree(struct ncclConnector* send) {
  struct connectMap* map = (struct connectMap*)(send->transportResources);
  if (map->sameProcess == 0) {
    NCCLCHECK(ncclShmClose(map->mems[NCCL_NET_MAP_HOSTMEM].cpuPtr, map->mems[NCCL_NET_MAP_HOSTMEM].gpuPtr, map->mems[NCCL_NET_MAP_HOSTMEM].size));
    if (map->mems[NCCL_NET_MAP_DEVMEM].size) {
      CUDACHECK(hipIpcCloseMemHandle(map->mems[NCCL_NET_MAP_DEVMEM].gpuPtr));
    }
  }
  return ncclSuccess;
}

static ncclResult_t recvFree(struct ncclConnector* recv) {
  return ncclSuccess;
}

#define NCCL_SHARED_STEPS 16
static ncclResult_t sharedBuffersInit(struct ncclComm* comm, int cuda, int localRank, int type, int sameProcess,
    int nChannels, char** gpuPtr, char** cpuPtr, int* size, hipIpcMemHandle_t* ipc) {
  if (cuda == 0 && sameProcess == 0) {
      WARN("PXN should not use host buffers for data");
      return ncclInternalError;
  }
  struct ncclProxyProgressState* progressState = &comm->proxyState.progressState;
  if (progressState->localPeers == NULL) {
    NCCLCHECK(ncclCalloc(&progressState->localPeers, comm->localRanks));
  }
  struct ncclProxyPeer** localPeers = progressState->localPeers;
  if (localPeers[localRank] == NULL) {
    NCCLCHECK(ncclCalloc(localPeers+localRank, 1));
  }
  struct ncclProxyPeer* peer = localPeers[localRank];
  struct ncclProxySharedP2p* state = type == 0 ? &peer->send : &peer->recv;
  state->refcount++;
  if (state->size == 0) {
    state->size = nChannels*(NCCL_SHARED_STEPS/NCCL_STEPS)*comm->buffSizes[NCCL_PROTO_SIMPLE]/SENDRECV_SLICEFACTOR;
  }

  if (size) *size = state->size;

  if (cuda && state->cudaBuff == NULL) {
    NCCLCHECK(ncclCudaCalloc(&state->cudaBuff, state->size, cuda));
    if (sameProcess == 0) {
      CUDACHECK(hipIpcGetMemHandle(&state->ipc, state->cudaBuff));
    }
  }
  if (!cuda && state->hostBuff == NULL) {
    NCCLCHECK(ncclCudaHostCalloc(&state->hostBuff, state->size));
  }
  if (cpuPtr) *cpuPtr = cuda ? state->cudaBuff : state->hostBuff;
  if (sameProcess) {
    if (gpuPtr) *gpuPtr = *cpuPtr;
  } else {
    if (gpuPtr) *gpuPtr = NULL;
    if (ipc) memcpy(ipc, &state->ipc, sizeof(hipIpcMemHandle_t));
  }
  return ncclSuccess;
}

static ncclResult_t sharedBuffersGet(struct ncclComm* comm, int channel, int slot, int* offset) {
  // Use different pools for different channels and also separate send/recv.
  int slotSize = comm->buffSizes[NCCL_PROTO_SIMPLE]/(NCCL_STEPS*SENDRECV_SLICEFACTOR);
  int globalSlot = (channel*NCCL_SHARED_STEPS)+slot;
  *offset = slotSize * globalSlot;
  return ncclSuccess;
}

static ncclResult_t sharedBuffersDestroy(struct ncclComm* comm, int localRank, int type) {
  if (comm->proxyState.progressState.localPeers == NULL) NCCLCHECK(ncclInternalError);
  struct ncclProxyPeer* peer = comm->proxyState.progressState.localPeers[localRank];
  if (peer == NULL) NCCLCHECK(ncclInternalError;)
  struct ncclProxySharedP2p* state = type == 0 ? &peer->send : &peer->recv;
  if (state->size == 0) NCCLCHECK(ncclInternalError);
  state->refcount--;
  if (state->refcount == 0) {
    if (state->cudaBuff) CUDACHECK(hipFree(state->cudaBuff));
    if (state->hostBuff) NCCLCHECK(ncclCudaHostFree(state->hostBuff));
  }
  if (peer->send.refcount || peer->recv.refcount) return ncclSuccess;
  free(peer);
  comm->proxyState.progressState.localPeers[localRank] = NULL;
  for (int r=0; r<comm->localRanks; r++) {
    if (comm->proxyState.progressState.localPeers[r]) return ncclSuccess;
  }
  // All peers are freed, free array
  free(comm->proxyState.progressState.localPeers);
  comm->proxyState.progressState.localPeers = NULL;
  return ncclSuccess;
}

static ncclResult_t proxySharedInit(struct ncclProxyConnection* connection, struct ncclComm* comm, int nChannels) {
  int rank = comm->localRankToRank[connection->localRank];
  int sameProcess = comm->peerInfo[rank].pidHash == comm->peerInfo[comm->rank].pidHash ? 1 : 0;
  NCCLCHECK(sharedBuffersInit(comm, 1, connection->localRank, 0, sameProcess, nChannels, NULL, NULL, NULL, NULL));
  return ncclSuccess;
}

static ncclResult_t sendProxySetup(struct ncclProxyConnection* connection, struct ncclComm* comm, void* reqBuff, int reqSize, void* respBuff, int respSize, int* done) {
  struct setupReq* req = (struct setupReq*) reqBuff;
  if (reqSize != sizeof(struct setupReq)) return ncclInternalError;

  struct sendResources* resources;
  NCCLCHECK(ncclCalloc(&resources, 1));
  connection->transportResources = resources;

  resources->rank = req->rank;
  resources->localRank = req->localRank;
  resources->remoteRank = req->remoteRank;
  resources->netDev = req->netDev;
  resources->shared = connection->shared = req->shared;
  resources->useGdr = req->useGdr;
  resources->channelId = req->channelId;
  resources->connIndex = req->connIndex;
  ncclNetProperties_t props;
  NCCLCHECK(ncclNetGetProperties(req->netDev, &props));
  resources->maxRecvs = props.maxRecvs;

  // We don't return any data
  if (respSize != 0) return ncclInternalError;
  *done = 1;
  return ncclSuccess;
}

static ncclResult_t recvProxySetup(struct ncclProxyConnection* connection, struct ncclComm* comm, void* reqBuff, int reqSize, void* respBuff, int respSize, int* done) {
  struct setupReq* req = (struct setupReq*) reqBuff;
  if (reqSize != sizeof(struct setupReq)) return ncclInternalError;

  struct recvResources* resources;
  NCCLCHECK(ncclCalloc(&resources, 1));
  connection->transportResources = resources;

  resources->rank = req->rank;
  resources->localRank = req->localRank;
  resources->remoteRank = req->remoteRank;
  resources->netDev = req->netDev;
  resources->shared = connection->shared = req->shared;
  resources->useGdr = req->useGdr;
  resources->channelId = req->channelId;
  resources->connIndex = req->connIndex;
  ncclNetProperties_t props;
  NCCLCHECK(ncclNetGetProperties(req->netDev, &props));
  resources->maxRecvs = props.maxRecvs;

  if (respSize != sizeof(ncclNetHandle_t)) return ncclInternalError;
  NCCLCHECK(ncclNetListen(req->netDev, respBuff, &resources->netListenComm));
  *done = 1;
  return ncclSuccess;
}

static ncclResult_t sendProxyConnect(struct ncclProxyConnection* connection, struct ncclComm* comm, void* reqBuff, int reqSize, void* respBuff, int respSize, int* done) {
  struct sendResources* resources = (struct sendResources*)(connection->transportResources);
  if (reqSize != sizeof(ncclNetHandle_t)) return ncclInternalError;

  if (resources->shared) {
    // Shared buffers
    struct ncclProxyProgressState* progressState = &comm->proxyState.progressState;
    if (progressState->localPeers == NULL) {
      NCCLCHECK(ncclCalloc(&progressState->localPeers, comm->localRanks));
    }
    struct ncclProxyPeer** localPeers = progressState->localPeers;
    if (localPeers[resources->localRank] == NULL) {
      NCCLCHECK(ncclCalloc(localPeers+resources->localRank, 1));
    }
    connection->proxyAppendPtr = localPeers[resources->localRank]->send.proxyAppend+resources->channelId;

    if (resources->maxRecvs > 1 && ncclParamNetSharedComms()) {
      // Connect or reuse connection for a netdev/remote rank.
      if (progressState->netComms[resources->netDev] == NULL) {
        NCCLCHECK(ncclCalloc(progressState->netComms+resources->netDev, comm->nRanks));
      }
      struct ncclSharedNetComms* comms = progressState->netComms[resources->netDev]+resources->remoteRank;
      if (comms->sendComm[resources->channelId] == NULL) NCCLCHECK(ncclNetConnect(resources->netDev, reqBuff, comms->sendComm+resources->channelId));
      resources->netSendComm = comms->sendComm[resources->channelId];
      if (comms->sendComm[resources->channelId]) comms->sendRefCount[resources->channelId]++;
    } else {
      NCCLCHECK(ncclNetConnect(resources->netDev, reqBuff, &resources->netSendComm));
    }
  } else {
    // Connect to remote peer
    NCCLCHECK(ncclNetConnect(resources->netDev, reqBuff, &resources->netSendComm));
    connection->proxyAppendPtr = &connection->proxyAppend;
  }
  if (resources->netSendComm == NULL) {
    *done = 0;
    return ncclSuccess;
  }
  *done = 1;

  // Create structures
  struct connectMap* map = &resources->map;
  map->sameProcess =
    comm->peerInfo[resources->rank].pidHash == comm->peerInfo[comm->rank].pidHash ? 1 : 0;
  map->shared = resources->shared;
  CUDACHECK(hipGetDevice(&map->cudaDev));

  if (resources->shared == 0) { // Only allocate dedicated buffers for ring/tree, not for p2p
    for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
      NCCL_NET_MAP_ADD_POINTER(map, 0, p!= NCCL_PROTO_LL && resources->useGdr, comm->buffSizes[p], buffs[p]);
      resources->buffSizes[p] = comm->buffSizes[p];
    }
  } else {
    // Get shared buffers
    int bank = resources->useGdr ? NCCL_NET_MAP_SHARED_DEVMEM : NCCL_NET_MAP_SHARED_HOSTMEM;
    struct connectMapMem* mapMem = map->mems+bank;
    NCCLCHECK(sharedBuffersInit(
          comm, resources->useGdr, resources->localRank, 0, map->sameProcess, comm->p2pnChannels,
          &mapMem->gpuPtr, &mapMem->cpuPtr, &mapMem->size, &mapMem->ipc));
    resources->buffSizes[NCCL_PROTO_SIMPLE] = mapMem->size;
    NCCL_NET_MAP_ADD_POINTER(map, 1, resources->useGdr, mapMem->size, buffs[NCCL_PROTO_SIMPLE]);
  }

  NCCL_NET_MAP_ADD_POINTER(map, 0, 0, sizeof(struct ncclSendMem), sendMem);
  NCCL_NET_MAP_ADD_POINTER(map, 0, 0, sizeof(struct ncclRecvMem), recvMem);

  if (map->mems[NCCL_NET_MAP_DEVMEM].size) {
    if (resources->shared == 0) {
      if (!map->sameProcess) {
        ALIGN_SIZE(map->mems[NCCL_NET_MAP_DEVMEM].size, CUDA_IPC_MIN);
      }
      NCCLCHECK(ncclCudaCalloc(&map->mems[NCCL_NET_MAP_DEVMEM].gpuPtr, map->mems[NCCL_NET_MAP_DEVMEM].size, resources->useGdr));
      map->mems[NCCL_NET_MAP_DEVMEM].cpuPtr = map->mems[NCCL_NET_MAP_DEVMEM].gpuPtr;
    }
    if (!map->sameProcess) {
      CUDACHECK(hipIpcGetMemHandle(&map->mems[NCCL_NET_MAP_DEVMEM].ipc, map->mems[NCCL_NET_MAP_DEVMEM].gpuPtr));
    }
  }
  if (map->sameProcess) {
    NCCLCHECK(ncclCudaHostCalloc(&map->mems[NCCL_NET_MAP_HOSTMEM].cpuPtr, map->mems[NCCL_NET_MAP_HOSTMEM].size));
    map->mems[NCCL_NET_MAP_HOSTMEM].gpuPtr = map->mems[NCCL_NET_MAP_HOSTMEM].cpuPtr;
  } else {
    NCCLCHECK(netCreateShm(map->mems+NCCL_NET_MAP_HOSTMEM));
  }
  if (ncclGdrCopy && map->sameProcess && ncclParamGdrCopySyncEnable()) {
    uint64_t *cpuPtr, *gpuPtr;
    NCCLCHECK(ncclGdrCudaCalloc(&cpuPtr, &gpuPtr, 1, &resources->gdrDesc));

    resources->gdcSync = cpuPtr;
    struct connectMapMem* gdcMem = map->mems+NCCL_NET_MAP_GDCMEM;
    gdcMem->cpuPtr = (char*)cpuPtr;
    gdcMem->gpuPtr = (char*)gpuPtr;
    gdcMem->size = sizeof(uint64_t); // sendMem->head
  }

  resources->sendMem = (struct ncclSendMem*) NCCL_NET_MAP_GET_POINTER(map, cpu, sendMem);
  resources->recvMem = (struct ncclRecvMem*) NCCL_NET_MAP_GET_POINTER(map, cpu, recvMem);

  // Don't give credits yet in shared mode.
  resources->sendMem->head = map->shared ? -NCCL_STEPS : 0;
  for (int i=0; i<NCCL_STEPS; i++) resources->recvMem->sizesFifo[i] = -1;

  for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
    resources->buffers[p] = NCCL_NET_MAP_GET_POINTER(map, cpu, buffs[p]);
    if (resources->buffers[p]) {
      NCCLCHECK(ncclNetRegMr(resources->netSendComm, resources->buffers[p], resources->buffSizes[p], NCCL_NET_MAP_DEV_MEM(map, buffs[p]) ? NCCL_PTR_CUDA : NCCL_PTR_HOST, &resources->mhandles[p]));
    }
  }

  //NCCLCHECK(netDumpMap(map));
  if (respSize != sizeof(struct connectMap)) return ncclInternalError;
  memcpy(respBuff, map, sizeof(struct connectMap));
  return ncclSuccess;
}

static ncclResult_t recvProxyConnect(struct ncclProxyConnection* connection, struct ncclComm* comm, void* reqBuff, int reqSize, void* respBuff, int respSize, int* done) {
  if (reqSize != sizeof(int)) return ncclInternalError;
  struct recvResources* resources = (struct recvResources*)(connection->transportResources);
  resources->proxyRank = *(int*)reqBuff;

  // Finish connection establishment from remote peer
  if (resources->shared) {
    // Shared buffers
    struct ncclProxyProgressState* progressState = &comm->proxyState.progressState;
    if (progressState->localPeers == NULL) {
      NCCLCHECK(ncclCalloc(&progressState->localPeers, comm->localRanks));
    }
    struct ncclProxyPeer** localPeers = progressState->localPeers;
    if (localPeers[resources->localRank] == NULL) {
      NCCLCHECK(ncclCalloc(localPeers+resources->localRank, 1));
    }
    connection->proxyAppendPtr = localPeers[resources->localRank]->recv.proxyAppend+resources->channelId;

    if (resources->maxRecvs > 1 && ncclParamNetSharedComms()) {
      // Connect or reuse connection for a netdev/remote rank.
      if (progressState->netComms[resources->netDev] == NULL) {
        NCCLCHECK(ncclCalloc(progressState->netComms+resources->netDev, comm->nRanks));
      }
      struct ncclSharedNetComms* comms = progressState->netComms[resources->netDev]+resources->proxyRank;
      if (comms->recvComm[resources->channelId] == NULL) NCCLCHECK(ncclNetAccept(resources->netListenComm, comms->recvComm+resources->channelId));
      resources->netRecvComm = comms->recvComm[resources->channelId];
      if (comms->recvComm[resources->channelId]) comms->recvRefCount[resources->channelId]++;
    } else {
      NCCLCHECK(ncclNetAccept(resources->netListenComm, &resources->netRecvComm));
    }
  } else {
    // Connect to remote peer
    NCCLCHECK(ncclNetAccept(resources->netListenComm, &resources->netRecvComm));
    connection->proxyAppendPtr = &connection->proxyAppend;
  }
  if (resources->netRecvComm == NULL) {
    *done = 0;
    return ncclSuccess;
  }
  *done = 1;
  NCCLCHECK(ncclNetCloseListen(resources->netListenComm));

  // Create structures
  struct connectMap* map = &resources->map;
  map->sameProcess =
    comm->peerInfo[resources->rank].pidHash == comm->peerInfo[comm->rank].pidHash ? 1 : 0;
  if (map->sameProcess == 0) return ncclInternalError; // We don't support remote proxy for recv
  map->shared = resources->shared;

  if (resources->shared == 0) { // Only allocate dedicated buffers for ring/tree, not for p2p
    for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
      NCCL_NET_MAP_ADD_POINTER(map, 0, resources->useGdr, comm->buffSizes[p], buffs[p]);
      resources->buffSizes[p] = comm->buffSizes[p];
    }
  } else {
    // Get shared buffers
    int bank = resources->useGdr ? NCCL_NET_MAP_SHARED_DEVMEM : NCCL_NET_MAP_SHARED_HOSTMEM;
    struct connectMapMem* mapMem = map->mems+bank;
    NCCLCHECK(sharedBuffersInit(
          comm, resources->useGdr, resources->localRank, 1, 1, comm->p2pnChannels,
          &mapMem->gpuPtr, &mapMem->cpuPtr, &mapMem->size, NULL));
    resources->buffSizes[NCCL_PROTO_SIMPLE] = mapMem->size;
    NCCL_NET_MAP_ADD_POINTER(map, 1, resources->useGdr, mapMem->size, buffs[NCCL_PROTO_SIMPLE]);
  }

  NCCL_NET_MAP_ADD_POINTER(map, 0, 0, sizeof(struct ncclSendMem), sendMem);
  NCCL_NET_MAP_ADD_POINTER(map, 0, 0, sizeof(struct ncclRecvMem), recvMem);

  if (map->mems[NCCL_NET_MAP_DEVMEM].size) {
    if (resources->shared == 0) {
      NCCLCHECK(ncclCudaCalloc(&map->mems[NCCL_NET_MAP_DEVMEM].gpuPtr, map->mems[NCCL_NET_MAP_DEVMEM].size, resources->useGdr));
      map->mems[NCCL_NET_MAP_DEVMEM].cpuPtr = map->mems[NCCL_NET_MAP_DEVMEM].gpuPtr;
    }
  }
  NCCLCHECK(ncclCudaHostCalloc(&map->mems[NCCL_NET_MAP_HOSTMEM].cpuPtr, map->mems[NCCL_NET_MAP_HOSTMEM].size));
  map->mems[NCCL_NET_MAP_HOSTMEM].gpuPtr = map->mems[NCCL_NET_MAP_HOSTMEM].cpuPtr;
  if (ncclGdrCopy && map->sameProcess) {
    uint64_t *cpuPtr, *gpuPtr;
    NCCLCHECK(ncclGdrCudaCalloc(&cpuPtr, &gpuPtr, 2, &resources->gdrDesc));

    if (ncclParamGdrCopySyncEnable()) {
      resources->gdcSync = cpuPtr;
      struct connectMapMem* gdcMem = map->mems+NCCL_NET_MAP_GDCMEM;
      gdcMem->cpuPtr = (char*)cpuPtr;
      gdcMem->gpuPtr = (char*)gpuPtr;
      gdcMem->size = sizeof(uint64_t);
    }
    if (ncclParamGdrCopyFlushEnable()) resources->gdcFlush = cpuPtr + 1;
  }

  resources->sendMem = (struct ncclSendMem*) NCCL_NET_MAP_GET_POINTER(map, cpu, sendMem);
  resources->recvMem = (struct ncclRecvMem*) NCCL_NET_MAP_GET_POINTER(map, cpu, recvMem);
  for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
    resources->buffers[p] = NCCL_NET_MAP_GET_POINTER(map, cpu, buffs[p]);
    if (resources->buffers[p]) {
      NCCLCHECK(ncclNetRegMr(resources->netRecvComm, resources->buffers[p], resources->buffSizes[p], NCCL_NET_MAP_DEV_MEM(map, buffs[p]) ? NCCL_PTR_CUDA : NCCL_PTR_HOST, &resources->mhandles[p]));
    }
  }

  //NCCLCHECK(netDumpMap(map));
  if (respSize != sizeof(struct connectMap)) return ncclInternalError;
  memcpy(respBuff, map, sizeof(struct connectMap));
  return ncclSuccess;
}

static ncclResult_t sendProxyFree(struct ncclProxyConnection* connection, struct ncclComm* comm) {
  struct sendResources* resources = (struct sendResources*)(connection->transportResources);
  if (resources == NULL) { // NVB Preconnect
    NCCLCHECK(sharedBuffersDestroy(comm, connection->localRank, 0));
    return ncclSuccess;
  }
  for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
    if (resources->buffers[p]) {
      NCCLCHECK(ncclNetDeregMr(resources->netSendComm, resources->mhandles[p]));
    }
  }
  struct connectMapMem* mems = resources->map.mems;
  if (resources->map.sameProcess) {
    NCCLCHECK(ncclCudaHostFree(mems[NCCL_NET_MAP_HOSTMEM].cpuPtr));
  } else {
    NCCLCHECK(ncclShmClose(mems[NCCL_NET_MAP_HOSTMEM].cpuPtr, NULL, mems[NCCL_NET_MAP_HOSTMEM].size));
  }
  CUDACHECK(hipFree(mems[NCCL_NET_MAP_DEVMEM].cpuPtr));
  if (mems[NCCL_NET_MAP_GDCMEM].cpuPtr) NCCLCHECK(ncclGdrCudaFree(resources->gdrDesc));
  if (resources->shared) {
    NCCLCHECK(sharedBuffersDestroy(comm, resources->localRank, 0));
    if (resources->maxRecvs > 1 && ncclParamNetSharedComms()) {
      struct ncclSharedNetComms* comms = comm->proxyState.progressState.netComms[resources->netDev]+resources->remoteRank;
      comms->sendRefCount[resources->channelId]--;
      if (comms->sendRefCount[resources->channelId] == 0) NCCLCHECK(ncclNetCloseSend(comms->sendComm[resources->channelId]));
    } else {
      NCCLCHECK(ncclNetCloseSend(resources->netSendComm));
    }
  } else {
    NCCLCHECK(ncclNetCloseSend(resources->netSendComm));
  }
  free(resources);
  return ncclSuccess;
}

static ncclResult_t recvProxyFree(struct ncclProxyConnection* connection, struct ncclComm* comm) {
  struct recvResources* resources = (struct recvResources*)(connection->transportResources);
  if (resources == NULL) { // NVB Preconnect
    NCCLCHECK(sharedBuffersDestroy(comm, connection->localRank, 1));
    return ncclSuccess;
  }
  for (int p=0; p<NCCL_NUM_PROTOCOLS; p++) {
    if (resources->buffers[p]) {
      NCCLCHECK(ncclNetDeregMr(resources->netRecvComm, resources->mhandles[p]));
    }
  }
  struct connectMapMem* mems = resources->map.mems;
  NCCLCHECK(ncclCudaHostFree(mems[NCCL_NET_MAP_HOSTMEM].cpuPtr));
  CUDACHECK(hipFree(mems[NCCL_NET_MAP_DEVMEM].cpuPtr));
  if (mems[NCCL_NET_MAP_GDCMEM].cpuPtr) NCCLCHECK(ncclGdrCudaFree(resources->gdrDesc));
  if (resources->shared) {
    NCCLCHECK(sharedBuffersDestroy(comm, resources->localRank, 1));
    if (resources->maxRecvs > 1 && ncclParamNetSharedComms()) {
      struct ncclSharedNetComms* comms = comm->proxyState.progressState.netComms[resources->netDev]+resources->proxyRank;
      comms->recvRefCount[resources->channelId]--;
      if (comms->recvRefCount[resources->channelId] == 0) NCCLCHECK(ncclNetCloseRecv(comms->recvComm[resources->channelId]));
    } else {
      NCCLCHECK(ncclNetCloseRecv(resources->netRecvComm));
    }
  } else {
    NCCLCHECK(ncclNetCloseRecv(resources->netRecvComm));
  }
  free(resources);
  return ncclSuccess;
}

static_assert(NCCL_STEPS <= NCCL_NET_MAX_REQUESTS, "Not enough net requests to cover for steps");

static ncclResult_t sendProxyProgress(struct ncclComm* comm, struct ncclProxyArgs* args) {
  if (args->state == ncclProxyOpReady) {
    for (int s=0; s<args->nsubs; s++) {
      struct ncclProxySubArgs* sub = args->subs+s;
      struct sendResources* resources = (struct sendResources*) (sub->connection->transportResources);
      // Round to next multiple of sliceSteps
      sub->base = ROUNDUP(resources->step, args->chunkSteps);
      sub->posted = sub->transmitted = sub->done = 0;
      for (uint64_t step=0; step<sub->nsteps; step++) ncclProfilingRecord(args, s, step, ncclProxyProfileBegin);
    }
    args->state = ncclProxyOpProgress;
    args->hdp_flushed = 0;
  }
  args->idle = 1;
  if (args->state == ncclProxyOpProgress) {
    int p = args->protocol;
    int maxDepth = std::min(NCCL_STEPS, NCCL_SHARED_STEPS/args->nsubs);
    for (int s=0; s<args->nsubs; s++) {
      struct ncclProxySubArgs* sub = args->subs+s;
      if (sub->done == sub->nsteps) continue;
      struct sendResources* resources = (struct sendResources*) (sub->connection->transportResources);
      void* mhandle = resources->mhandles[p];
      int stepSize = resources->buffSizes[p] / NCCL_STEPS;
      char* localBuff = NCCL_NET_MAP_GET_POINTER(&resources->map, cpu, buffs[p]);
      int buffSize = stepSize*args->sliceSteps;
      if (sub->nbytes < buffSize) buffSize = sub->nbytes;
      // Post buffers to the GPU
      if (sub->posted < sub->nsteps && sub->posted < sub->done + maxDepth) {
        int buffSlot = (sub->base+sub->posted)%NCCL_STEPS;
        if (resources->shared) {
          int sharedBuffSlot = sub->posted%maxDepth;
          int offset;
          NCCLCHECK(sharedBuffersGet(comm, sub->channelId, sharedBuffSlot*args->nsubs+s, &offset));
          resources->recvMem->offsFifo[buffSlot] = offset;
          __sync_synchronize();
          volatile uint64_t* sendHead = resources->gdcSync ? resources->gdcSync : &resources->sendMem->head;
          sub->posted += args->sliceSteps;
          *sendHead = sub->base + sub->posted - NCCL_STEPS;
          if (resources->gdcSync) wc_store_fence(); // Flush out WC write
        } else sub->posted += args->sliceSteps;
        for (uint64_t step=sub->posted-args->sliceSteps; step<sub->posted; step++) {
          ncclProfilingRecord(args, s, step, ncclProxyProfileSendGPUWait);
        }
        args->idle = 0;
        continue;
      }
      // Check whether we received data from the GPU and send it to the network
      if (sub->transmitted < sub->posted && sub->transmitted < sub->done + NCCL_STEPS) {
        int buffSlot = (sub->base+sub->transmitted)%NCCL_STEPS;
        volatile int* sizesFifo = resources->recvMem->sizesFifo;
        volatile uint64_t* recvTail = &resources->recvMem->tail;
        if (sizesFifo[buffSlot] != -1 && ((*recvTail > (sub->base+sub->transmitted)) || p == NCCL_PROTO_LL)) {
          // We have something to receive, let's check if it's completely ready.
          int size = sizesFifo[buffSlot];
          char* buff = resources->shared ? localBuff+resources->recvMem->offsFifo[buffSlot] : localBuff+buffSlot*stepSize;
          int ready = 1;
          if (p == NCCL_PROTO_LL128) {
            ready = resources->useGdr;
            if (!ready) {
              // When data is in sysmem, we need to wait until all flags are correct since the GPU only
              // called threadfence()
              uint64_t flag = sub->base+sub->transmitted+1;
              int nFifoLines = DIVUP(sizesFifo[buffSlot], sizeof(uint64_t)*NCCL_LL128_LINEELEMS);
              volatile uint64_t* lines = (volatile uint64_t*)buff;
              ready = 1;
              for (int i=0; i<nFifoLines; i++) {
                if (lines[i*NCCL_LL128_LINEELEMS+NCCL_LL128_DATAELEMS] != flag) { ready = 0; break; }
              }
            }
          } else if (p == NCCL_PROTO_LL) {
            uint32_t flag = NCCL_LL_FLAG(sub->base+sub->transmitted+1);
            int nFifoLines = DIVUP(size, sizeof(union ncclLLFifoLine));
            union ncclLLFifoLine* lines = (union ncclLLFifoLine*)buff;
            for (int i=0; i<nFifoLines; i++) {
              volatile uint32_t *f1 = &lines[i].flag1;
              volatile uint32_t *f2 = &lines[i].flag2;
              if (f1[0] != flag || f2[0] != flag) { ready = 0; break; }
            }
          }
          if (ready) {
            // flush HDP if not done
            if (resources->curr_hdp_reg && args->hdp_flushed < LOAD(recvTail)) {
              args->hdp_flushed = LOAD(recvTail);
              STORE(resources->curr_hdp_reg, 1);
            }
            // Data is ready, try to send.
            NCCLCHECK(ncclNetIsend(resources->netSendComm, buff, size, resources->rank, mhandle, sub->requests+buffSlot));
            if (sub->requests[buffSlot] != NULL) {
              TRACE(NCCL_NET, "sendProxy [%ld/%d] Isend posted, req %p", sub->transmitted, buffSlot, sub->requests[buffSlot]);
              sizesFifo[buffSlot] = -1;
              // Make sure size is reset to zero before we update the head.
              __sync_synchronize();
              sub->transmitted += args->sliceSteps;
              for (uint64_t step=sub->transmitted-args->sliceSteps; step<sub->transmitted; step++) ncclProfilingRecord(args, s, step, ncclProxyProfileSendWait);
              args->idle = 0;
              continue;
            }
          }
        }
      }
      // Check whether the network has completed some send operations.
      if (sub->done < sub->transmitted) {
        int done;
        int buffSlot = (sub->base+sub->done)%NCCL_STEPS;
        NCCLCHECK(ncclNetTest(sub->requests[buffSlot], &done, NULL));
        if (done) {
          TRACE(NCCL_NET, "sendProxy [%ld/%d] request %p done", sub->done, buffSlot, sub->requests[buffSlot]);
          sub->done += args->sliceSteps;
          for (uint64_t step=sub->done-args->sliceSteps; step<sub->done; step++) ncclProfilingRecord(args, s, step, ncclProxyProfileEnd);

          if (resources->shared == 0) {
            volatile uint64_t* sendHead = resources->gdcSync ? resources->gdcSync : &resources->sendMem->head;
            *sendHead = sub->base + sub->done;
            if (resources->gdcSync) wc_store_fence(); // Flush out WC write
          }
          args->idle = 0;
          if (sub->done == sub->nsteps) {
            resources->step = sub->base + sub->nsteps;
            args->done++;
          }
        }
      }
    }
    if (args->done == args->nsubs) {
      args->state = ncclProxyOpNone;
    }
  }
  return ncclSuccess;
}

static ncclResult_t recvProxyProgress(struct ncclComm* comm, struct ncclProxyArgs* args) {
  if (args->state == ncclProxyOpReady) {
    // Initialize subs and group them by same recvComm.
    void* recvComm;
    int groupSize = 0;
    int maxRecvs = 1;
    for (int s=0; s<args->nsubs; s++) {
      struct ncclProxySubArgs* sub = args->subs+s;
      if (groupSize == maxRecvs) {
        groupSize = 0;
      } else if (s>0) { // Find next sub with the same recvComm
        int next;
        for (next=s; next<args->nsubs; next++) {
          struct recvResources* nextRes = (struct recvResources*) (args->subs[next].connection->transportResources);
          if (nextRes->netRecvComm == recvComm) break;
        }
        if (next == args->nsubs) { // Not found
          groupSize = 0;
        } else if (s != next) { // We found a sub later with the same recvComm ; swap subs
          struct ncclProxySubArgs temp;
          memcpy(&temp, sub, sizeof(struct ncclProxySubArgs));
          memcpy(sub, args->subs+next, sizeof(struct ncclProxySubArgs));
          memcpy(args->subs+next, &temp, sizeof(struct ncclProxySubArgs));
        }
      }
      groupSize++;
      struct recvResources* resources = (struct recvResources*) (sub->connection->transportResources);
      maxRecvs = resources->maxRecvs;
      recvComm = resources->netRecvComm;
      // Round to next multiple of sliceSteps
      sub->base = ROUNDUP(resources->step, args->chunkSteps);
      sub->posted = sub->received = sub->transmitted = sub->done = 0;
      for (int i=0; i<groupSize; i++) sub[-i].groupSize = groupSize;
      for (uint64_t step=0; step<sub->nsteps; step++) ncclProfilingRecord(args, s, step, ncclProxyProfileBegin);
    }
    args->state = ncclProxyOpProgress;
  }
  args->idle = 1;
  if (args->state == ncclProxyOpProgress) {
    int p = args->protocol;
    int maxDepth = std::min(NCCL_STEPS, NCCL_SHARED_STEPS/args->nsubs);
    for (int s=0; s<args->nsubs; s+=args->subs[s].groupSize) {
      struct ncclProxySubArgs* subGroup = args->subs+s;
      int subCount = 0;
      void* ptrs[NCCL_PROXY_MAX_SUBS];
      int sizes[NCCL_PROXY_MAX_SUBS];
      int tags[NCCL_PROXY_MAX_SUBS];
      void* mhandles[NCCL_PROXY_MAX_SUBS];

      for (int i=0; i<subGroup->groupSize; i++) {
        struct ncclProxySubArgs* sub = subGroup + i;
        if (sub->posted < sub->nsteps) {
          if (sub->posted >= sub->done + maxDepth) { subCount = 0; break; }
          struct recvResources* resources = (struct recvResources*) (sub->connection->transportResources);
          int stepSize = resources->buffSizes[p] / NCCL_STEPS;
          char* localBuff = NCCL_NET_MAP_GET_POINTER(&resources->map, cpu, buffs[p]);
          int buffSlot = (sub->base+sub->posted)%NCCL_STEPS;
          if (resources->shared) {
            int sharedBuffSlot = sub->posted%maxDepth;
            int offset;
            NCCLCHECK(sharedBuffersGet(comm, sub->channelId, sharedBuffSlot*args->nsubs+s+i, &offset));
            volatile int* offsFifo = (volatile int*)resources->recvMem->offsFifo;
            offsFifo[buffSlot] = offset;
            ptrs[subCount] = localBuff+offset;
          } else {
            ptrs[subCount] = localBuff+buffSlot*stepSize;
          }
          sizes[subCount] = stepSize*args->sliceSteps;
          if (sub->nbytes < sizes[subCount]) sizes[subCount] = sub->nbytes;
          tags[subCount] = resources->remoteRank;
          mhandles[subCount] = resources->mhandles[p];
          subCount++;
        }
      }
      if (subCount) {
        uint64_t step = subGroup->posted;
        struct recvResources* resources = (struct recvResources*) (subGroup->connection->transportResources);
        void** requestPtr = subGroup->requests+(step%NCCL_STEPS);
        NCCLCHECK(ncclNetIrecv(resources->netRecvComm, subCount, ptrs, sizes, tags, mhandles, requestPtr));
        if (*requestPtr) {
          for (int i=0; i<subGroup->groupSize; i++) {
            struct ncclProxySubArgs* sub = subGroup+i;
            sub->posted += args->sliceSteps;
            for (uint64_t step=sub->posted-args->sliceSteps; step<sub->posted; step++) ncclProfilingRecord(args, s+i, step, ncclProxyProfileRecvWait);
          }
          args->idle = 0;
        }
      }
    }
    if (args->idle == 0) return ncclSuccess;

    for (int s=0; s<args->nsubs; s+=args->subs[s].groupSize) {
      struct ncclProxySubArgs* subGroup = args->subs+s;
      if (subGroup->posted > subGroup->received) {
        uint64_t step = subGroup->received;
        int done;
        void* ptrs[NCCL_PROXY_MAX_SUBS];
        int sizes[NCCL_PROXY_MAX_SUBS];
        void* mhandles[NCCL_PROXY_MAX_SUBS];
        for (int i=0; i<NCCL_PROXY_MAX_SUBS; i++) sizes[i] = 0;
        NCCLCHECK(ncclNetTest(subGroup->requests[step%NCCL_STEPS], &done, sizes));
        if (done) {
          int useGdr = 0;
          int totalSize = 0;
          for (int i=0; i<NCCL_PROXY_MAX_SUBS; i++) totalSize += sizes[i];
          for (int i=0; i<subGroup->groupSize; i++) {
            struct ncclProxySubArgs* sub = subGroup + i;
            sub->received += args->sliceSteps;
            for (uint64_t step=sub->received-args->sliceSteps; step<sub->received; step++) ncclProfilingRecord(args, s+i, step, ncclProxyProfileRecvFlushWait);
            if (step < sub->nsteps) {
              struct recvResources* resources = (struct recvResources*) (sub->connection->transportResources);
              if (resources->useGdr) useGdr = 1;
            }
          }
          subGroup->requests[step%NCCL_STEPS] = NULL;
          if (totalSize > 0 && p == NCCL_PROTO_SIMPLE && useGdr) {
            // GDRCOPY support
            struct recvResources* resources = (struct recvResources*) (subGroup->connection->transportResources);
            if (resources->gdcFlush) {
#if defined (__x86_64__)
              // Force a PCI-E read from GPU memory
              asm volatile ("mov (%0), %%eax" :: "l"(resources->gdcFlush) : "%eax");
#else
              WARN("NET: GDR Flush only supported on x86_64");
              return ncclInternalError;
#endif
            } else {
              int subCount = 0;
              for (int i=0; i<subGroup->groupSize; i++) {
                struct ncclProxySubArgs* sub = subGroup + i;
                if (step < sub->nsteps) {
                  struct recvResources* resources = (struct recvResources*) (sub->connection->transportResources);
                  int stepSize = resources->buffSizes[p] / NCCL_STEPS;
                  char* localBuff = NCCL_NET_MAP_GET_POINTER(&resources->map, cpu, buffs[p]);
                  int buffSlot = (sub->base+sub->posted)%NCCL_STEPS;
                  ptrs[subCount] = resources->shared ? localBuff+resources->recvMem->offsFifo[buffSlot] : localBuff+buffSlot*stepSize;
                  mhandles[subCount] = resources->mhandles[p];
                  subCount++;
                }
              }
              struct recvResources* resources = (struct recvResources*) (subGroup->connection->transportResources);
              NCCLCHECK(ncclNetIflush(resources->netRecvComm, subCount, ptrs, sizes, mhandles, subGroup->requests+(step%NCCL_STEPS)));
            }
          }
          args->idle = 0;
        }
      }
    }
    if (args->idle == 0) return ncclSuccess;

    for (int s=0; s<args->nsubs; s+=args->subs[s].groupSize) {
      struct ncclProxySubArgs* subGroup = args->subs+s;
      if (subGroup->received > subGroup->transmitted) {
        uint64_t step = subGroup->transmitted;
        int done = 1;
        void* request = subGroup->requests[step%NCCL_STEPS];
        if (request) NCCLCHECK(ncclNetTest(request, &done, NULL));
        if (done) {
          for (int i=0; i<subGroup->groupSize; i++) {
            struct ncclProxySubArgs* sub = subGroup + i;
            sub->transmitted += args->sliceSteps;
            for (uint64_t step=sub->transmitted-args->sliceSteps; step<sub->transmitted; step++) ncclProfilingRecord(args, s+i, step, ncclProxyProfileRecvGPUWait);
            if (step < sub->nsteps) {
              __sync_synchronize();
              struct recvResources* resources = (struct recvResources*) (sub->connection->transportResources);
              volatile uint64_t* recvTail = resources->gdcSync ? resources->gdcSync : &resources->recvMem->tail;
              *recvTail = sub->base + sub->transmitted;
              if (resources->gdcSync) wc_store_fence(); // Flush out WC write
            }
          }
          args->idle = 0;
        }
      }
    }
    if (args->idle == 0) return ncclSuccess;

    for (int s=0; s<args->nsubs; s+=args->subs[s].groupSize) {
      struct ncclProxySubArgs* subGroup = args->subs+s;
      for (int i=0; i<subGroup->groupSize; i++) {
        struct ncclProxySubArgs* sub = subGroup + i;
        if (sub->done == sub->nsteps) continue;
        if (sub->transmitted > sub->done) {
          struct recvResources* resources = (struct recvResources*) (sub->connection->transportResources);
          volatile uint64_t* sendHead = &resources->sendMem->head;
          uint64_t done = *sendHead;
          while (done > sub->base + sub->done &&
              // LL and LL128 can acknowledge 0-bytes send before they even happen. Don't go past what we transmitted.
              sub->transmitted > sub->done) {
            sub->done += args->sliceSteps;
            for (uint64_t step=sub->done-args->sliceSteps; step<sub->done; step++) ncclProfilingRecord(args, s+i, step, ncclProxyProfileEnd);
            args->idle = 0;
            if (sub->done == sub->nsteps) {
              struct recvResources* resources = (struct recvResources*) (sub->connection->transportResources);
              resources->step = sub->base + sub->nsteps;
              args->done++;
              break;
            }
          }
        }
      }
    }
    if (args->done == args->nsubs) {
      args->state = ncclProxyOpNone;
    }
  }
  return ncclSuccess;
}

struct ncclTransport netTransport = {
  "NET",
  canConnect,
  { sendSetup, sendConnect, sendFree, proxySharedInit, sendProxySetup, sendProxyConnect, sendProxyFree, sendProxyProgress },
  { recvSetup, recvConnect, recvFree, proxySharedInit, recvProxySetup, recvProxyConnect, recvProxyFree, recvProxyProgress }
};
