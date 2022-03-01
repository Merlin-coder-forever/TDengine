/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "syncMessage.h"
#include "syncRaft.h"
#include "tcoding.h"

void onMessage(SRaft* pRaft, void* pMsg) {}

// ---- message build ----
SyncPing* syncPingBuild(uint32_t dataLen) {
  uint32_t  bytes = SYNC_PING_FIX_LEN + dataLen;
  SyncPing* pSyncPing = malloc(bytes);
  memset(pSyncPing, 0, bytes);
  pSyncPing->bytes = bytes;
  pSyncPing->msgType = SYNC_PING;
  pSyncPing->dataLen = dataLen;
}

void syncPingDestroy(SyncPing* pSyncPing) {
  if (pSyncPing != NULL) {
    free(pSyncPing);
  }
}

void syncPingSerialize(const SyncPing* pSyncPing, char* buf, uint32_t bufLen) {
  assert(pSyncPing->bytes <= bufLen);
  memcpy(buf, pSyncPing, pSyncPing->bytes);
}

void syncPingDeserialize(const char* buf, uint32_t len, SyncPing* pSyncPing) {
  uint32_t* pU32 = (uint32_t*)buf;
  uint32_t  bytes = *pU32;
  pSyncPing = (SyncPing*)malloc(bytes);
  memcpy(pSyncPing, buf, len);
  assert(len == pSyncPing->bytes);
  assert(pSyncPing->bytes == SYNC_PING_FIX_LEN + pSyncPing->dataLen);
}

void syncPing2RpcMsg(const SyncPing* pSyncPing, SRpcMsg* pRpcMsg) {
  pRpcMsg->msgType = pSyncPing->msgType;
  uint32_t bufLen = pSyncPing->bytes;
  char*    buf = malloc(bufLen);
  syncPingSerialize(pSyncPing, buf, bufLen);
  pRpcMsg->contLen = bufLen;
  pRpcMsg->pCont = rpcMallocCont(pRpcMsg->contLen);
  memcpy(pRpcMsg->pCont, buf, pRpcMsg->contLen);
  free(buf);
}

/*
typedef struct SRaftId {
  SyncNodeId  addr;  // typedef uint64_t SyncNodeId;
  SyncGroupId vgId;  // typedef int32_t  SyncGroupId;
} SRaftId;

typedef struct SyncPing {
  uint32_t bytes;
  uint32_t msgType;
  SRaftId  srcId;
  SRaftId  destId;
  uint32_t dataLen;
  char*    data;
} SyncPing;
*/

/*
void syncPingSerialize(const SyncPing* pSyncPing, char** ppBuf, uint32_t* bufLen) {
  *bufLen = sizeof(SyncPing) + pSyncPing->dataLen;
  *ppBuf = (char*)malloc(*bufLen);
  void*    pStart = *ppBuf;
  uint32_t allBytes = *bufLen;

  int len = 0;
  len = taosEncodeFixedU32(&pStart, pSyncPing->msgType);
  allBytes -= len;
  assert(len > 0);
  pStart += len;

  len = taosEncodeFixedU64(&pStart, pSyncPing->srcId.addr);
  allBytes -= len;
  assert(len > 0);
  pStart += len;

  len = taosEncodeFixedI32(&pStart, pSyncPing->srcId.vgId);
  allBytes -= len;
  assert(len > 0);
  pStart += len;

  len = taosEncodeFixedU64(&pStart, pSyncPing->destId.addr);
  allBytes -= len;
  assert(len > 0);
  pStart += len;

  len = taosEncodeFixedI32(&pStart, pSyncPing->destId.vgId);
  allBytes -= len;
  assert(len > 0);
  pStart += len;

  len = taosEncodeFixedU32(&pStart, pSyncPing->dataLen);
  allBytes -= len;
  assert(len > 0);
  pStart += len;

  memcpy(pStart, pSyncPing->data, pSyncPing->dataLen);
  allBytes -= pSyncPing->dataLen;
  assert(allBytes == 0);
}


void syncPingDeserialize(const char* buf, uint32_t len, SyncPing* pSyncPing) {
  void*    pStart = (void*)buf;
  uint64_t u64;
  int32_t  i32;
  uint32_t u32;

  pStart = taosDecodeFixedU64(pStart, &u64);
  pSyncPing->msgType = u64;

  pStart = taosDecodeFixedU64(pStart, &u64);
  pSyncPing->srcId.addr = u64;

  pStart = taosDecodeFixedI32(pStart, &i32);
  pSyncPing->srcId.vgId = i32;

  pStart = taosDecodeFixedU64(pStart, &u64);
  pSyncPing->destId.addr = u64;

  pStart = taosDecodeFixedI32(pStart, &i32);
  pSyncPing->destId.vgId = i32;

  pStart = taosDecodeFixedU32(pStart, &u32);
  pSyncPing->dataLen = u32;
}
*/