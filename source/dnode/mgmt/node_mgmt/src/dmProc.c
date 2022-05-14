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

#define _DEFAULT_SOURCE
#include "dmMgmt.h"

static inline int32_t CEIL8(int32_t v) {
  const int32_t c = ceil((float)(v) / 8) * 8;
  return c < 8 ? 8 : c;
}

static int32_t dmInitProcMutex(SProcQueue *queue) {
  TdThreadMutexAttr mattr = {0};

  if (taosThreadMutexAttrInit(&mattr) != 0) {
    terrno = TAOS_SYSTEM_ERROR(errno);
    dError("node:%s, failed to init mutex while init attr since %s", queue->name, terrstr());
    return -1;
  }

  if (taosThreadMutexAttrSetPshared(&mattr, PTHREAD_PROCESS_SHARED) != 0) {
    taosThreadMutexAttrDestroy(&mattr);
    terrno = TAOS_SYSTEM_ERROR(errno);
    dError("node:%s, failed to init mutex while set shared since %s", queue->name, terrstr());
    return -1;
  }

  if (taosThreadMutexInit(&queue->mutex, &mattr) != 0) {
    taosThreadMutexAttrDestroy(&mattr);
    terrno = TAOS_SYSTEM_ERROR(errno);
    dError("node:%s, failed to init mutex since %s", queue->name, terrstr());
    return -1;
  }

  taosThreadMutexAttrDestroy(&mattr);
  return 0;
}

static int32_t dmInitProcSem(SProcQueue *queue) {
  if (tsem_init(&queue->sem, 1, 0) != 0) {
    terrno = TAOS_SYSTEM_ERROR(errno);
    dError("node:%s, failed to init sem since %s", queue->name, terrstr());
    return -1;
  }

  return 0;
}

static SProcQueue *dmInitProcQueue(SProc *proc, char *ptr, int32_t size) {
  SProcQueue *queue = (SProcQueue *)(ptr);

  int32_t bufSize = size - CEIL8(sizeof(SProcQueue));
  if (bufSize <= 1024) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }

  if (InParentProc(proc->ptype) && !InChildProc(proc->ptype)) {
    if (dmInitProcMutex(queue) != 0) {
      return NULL;
    }

    if (dmInitProcSem(queue) != 0) {
      return NULL;
    }

    tstrncpy(queue->name, proc->name, sizeof(queue->name));
    queue->head = 0;
    queue->tail = 0;
    queue->total = bufSize;
    queue->avail = bufSize;
    queue->items = 0;
  }

  return queue;
}

#if 0
static void dmDestroyProcQueue(SProcQueue *queue) {
  if (queue->mutex != NULL) {
    taosThreadMutexDestroy(queue->mutex);
    queue->mutex = NULL;
  }
}

static void dmDestroyProcSem(SProcQueue *queue) {
  if (queue->sem != NULL) {
    tsem_destroy(queue->sem);
    queue->sem = NULL;
  }
}
#endif

static void dmCleanupProcQueue(SProcQueue *queue) {
#if 0  
  if (queue != NULL) {
    dmDestroyProcQueue(queue);
    dmDestroyProcSem(queue);
  }
#endif
}

static int32_t dmPushToProcQueue(SProc *proc, SProcQueue *queue, const char *pHead, int16_t rawHeadLen,
                                 const char *pBody, int32_t rawBodyLen, int64_t handle, int64_t handleRef,
                                 EProcFuncType ftype) {
  if (rawHeadLen == 0 || pHead == NULL) {
    terrno = TSDB_CODE_INVALID_PARA;
    return -1;
  }

  const int32_t headLen = CEIL8(rawHeadLen);
  const int32_t bodyLen = CEIL8(rawBodyLen);
  const int32_t fullLen = headLen + bodyLen + 8;

  taosThreadMutexLock(&queue->mutex);
  if (fullLen > queue->avail) {
    taosThreadMutexUnlock(&queue->mutex);
    terrno = TSDB_CODE_OUT_OF_SHM_MEM;
    return -1;
  }

  if (handle != 0 && ftype == PROC_FUNC_REQ) {
    if (taosHashPut(proc->hash, &handle, sizeof(int64_t), &handleRef, sizeof(int64_t)) != 0) {
      taosThreadMutexUnlock(&queue->mutex);
      return -1;
    }
  }

  const int32_t pos = queue->tail;
  if (queue->tail < queue->total) {
    *(int16_t *)(queue->pBuffer + queue->tail) = rawHeadLen;
    *(int8_t *)(queue->pBuffer + queue->tail + 2) = (int8_t)ftype;
    *(int32_t *)(queue->pBuffer + queue->tail + 4) = rawBodyLen;
  } else {
    *(int16_t *)(queue->pBuffer) = rawHeadLen;
    *(int8_t *)(queue->pBuffer + 2) = (int8_t)ftype;
    *(int32_t *)(queue->pBuffer + 4) = rawBodyLen;
  }

  if (queue->tail < queue->head) {
    memcpy(queue->pBuffer + queue->tail + 8, pHead, rawHeadLen);
    memcpy(queue->pBuffer + queue->tail + 8 + headLen, pBody, rawBodyLen);
    queue->tail = queue->tail + 8 + headLen + bodyLen;
  } else {
    int32_t remain = queue->total - queue->tail;
    if (remain == 0) {
      memcpy(queue->pBuffer + 8, pHead, rawHeadLen);
      memcpy(queue->pBuffer + 8 + headLen, pBody, rawBodyLen);
      queue->tail = 8 + headLen + bodyLen;
    } else if (remain == 8) {
      memcpy(queue->pBuffer, pHead, rawHeadLen);
      memcpy(queue->pBuffer + headLen, pBody, rawBodyLen);
      queue->tail = headLen + bodyLen;
    } else if (remain < 8 + headLen) {
      memcpy(queue->pBuffer + queue->tail + 8, pHead, remain - 8);
      memcpy(queue->pBuffer, pHead + remain - 8, rawHeadLen - (remain - 8));
      memcpy(queue->pBuffer + headLen - (remain - 8), pBody, rawBodyLen);
      queue->tail = headLen - (remain - 8) + bodyLen;
    } else if (remain < 8 + headLen + bodyLen) {
      memcpy(queue->pBuffer + queue->tail + 8, pHead, rawHeadLen);
      memcpy(queue->pBuffer + queue->tail + 8 + headLen, pBody, remain - 8 - headLen);
      memcpy(queue->pBuffer, pBody + remain - 8 - headLen, rawBodyLen - (remain - 8 - headLen));
      queue->tail = bodyLen - (remain - 8 - headLen);
    } else {
      memcpy(queue->pBuffer + queue->tail + 8, pHead, rawHeadLen);
      memcpy(queue->pBuffer + queue->tail + headLen + 8, pBody, rawBodyLen);
      queue->tail = queue->tail + headLen + bodyLen + 8;
    }
  }

  queue->avail -= fullLen;
  queue->items++;
  taosThreadMutexUnlock(&queue->mutex);
  tsem_post(&queue->sem);

  dTrace("node:%s, push proc msg at pos:%d ftype:%d remain:%d handle:%p ref:%" PRId64 ", head:%d %p body:%d %p",
         queue->name, pos, ftype, queue->items, (void *)handle, handleRef, headLen, pHead, bodyLen, pBody);
  return 0;
}

static int32_t dmPopFromProcQueue(SProcQueue *queue, void **ppHead, int16_t *pHeadLen, void **ppBody, int32_t *pBodyLen,
                                  EProcFuncType *pFuncType) {
  tsem_wait(&queue->sem);

  taosThreadMutexLock(&queue->mutex);
  if (queue->total - queue->avail <= 0) {
    taosThreadMutexUnlock(&queue->mutex);
    terrno = TSDB_CODE_OUT_OF_SHM_MEM;
    return 0;
  }

  int16_t rawHeadLen = 0;
  int8_t  ftype = 0;
  int32_t rawBodyLen = 0;
  if (queue->head < queue->total) {
    rawHeadLen = *(int16_t *)(queue->pBuffer + queue->head);
    ftype = *(int8_t *)(queue->pBuffer + queue->head + 2);
    rawBodyLen = *(int32_t *)(queue->pBuffer + queue->head + 4);
  } else {
    rawHeadLen = *(int16_t *)(queue->pBuffer);
    ftype = *(int8_t *)(queue->pBuffer + 2);
    rawBodyLen = *(int32_t *)(queue->pBuffer + 4);
  }
  int16_t headLen = CEIL8(rawHeadLen);
  int32_t bodyLen = CEIL8(rawBodyLen);

  void *pHead = taosAllocateQitem(headLen, DEF_QITEM);
  void *pBody = rpcMallocCont(bodyLen);
  if (pHead == NULL || pBody == NULL) {
    taosThreadMutexUnlock(&queue->mutex);
    tsem_post(&queue->sem);
    taosFreeQitem(pHead);
    rpcFreeCont(pBody);
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }

  const int32_t pos = queue->head;
  if (queue->head < queue->tail) {
    memcpy(pHead, queue->pBuffer + queue->head + 8, headLen);
    memcpy(pBody, queue->pBuffer + queue->head + 8 + headLen, bodyLen);
    queue->head = queue->head + 8 + headLen + bodyLen;
  } else {
    int32_t remain = queue->total - queue->head;
    if (remain == 0) {
      memcpy(pHead, queue->pBuffer + 8, headLen);
      memcpy(pBody, queue->pBuffer + 8 + headLen, bodyLen);
      queue->head = 8 + headLen + bodyLen;
    } else if (remain == 8) {
      memcpy(pHead, queue->pBuffer, headLen);
      memcpy(pBody, queue->pBuffer + headLen, bodyLen);
      queue->head = headLen + bodyLen;
    } else if (remain < 8 + headLen) {
      memcpy(pHead, queue->pBuffer + queue->head + 8, remain - 8);
      memcpy((char *)pHead + remain - 8, queue->pBuffer, headLen - (remain - 8));
      memcpy(pBody, queue->pBuffer + headLen - (remain - 8), bodyLen);
      queue->head = headLen - (remain - 8) + bodyLen;
    } else if (remain < 8 + headLen + bodyLen) {
      memcpy(pHead, queue->pBuffer + queue->head + 8, headLen);
      memcpy(pBody, queue->pBuffer + queue->head + 8 + headLen, remain - 8 - headLen);
      memcpy((char *)pBody + remain - 8 - headLen, queue->pBuffer, bodyLen - (remain - 8 - headLen));
      queue->head = bodyLen - (remain - 8 - headLen);
    } else {
      memcpy(pHead, queue->pBuffer + queue->head + 8, headLen);
      memcpy(pBody, queue->pBuffer + queue->head + headLen + 8, bodyLen);
      queue->head = queue->head + headLen + bodyLen + 8;
    }
  }

  queue->avail = queue->avail + headLen + bodyLen + 8;
  queue->items--;
  taosThreadMutexUnlock(&queue->mutex);

  *ppHead = pHead;
  *ppBody = pBody;
  *pHeadLen = rawHeadLen;
  *pBodyLen = rawBodyLen;
  *pFuncType = (EProcFuncType)ftype;

  dTrace("proc:%s, pop msg at pos:%d ftype:%d remain:%d, head:%d %p body:%d %p", queue->name, pos, ftype, queue->items,
         rawHeadLen, pHead, rawBodyLen, pBody);
  return 1;
}

int32_t dmInitProc(struct SMgmtWrapper *pWrapper) {
  SProc *proc = &pWrapper->proc;
  proc->wrapper = pWrapper;
  proc->name = pWrapper->name;

  SShm   *shm = &proc->shm;
  int32_t cstart = 0;
  int32_t csize = CEIL8(shm->size / 2);
  int32_t pstart = csize;
  int32_t psize = CEIL8(shm->size - pstart);
  if (pstart + psize > shm->size) {
    psize -= 8;
  }

  proc->hash = taosHashInit(128, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), false, HASH_NO_LOCK);
  proc->cqueue = dmInitProcQueue(proc, (char *)shm->ptr + cstart, csize);
  proc->pqueue = dmInitProcQueue(proc, (char *)shm->ptr + pstart, psize);
  if (proc->cqueue == NULL || proc->pqueue == NULL || proc->hash == NULL) {
    dmCleanupProcQueue(proc->cqueue);
    dmCleanupProcQueue(proc->pqueue);
    taosHashCleanup(proc->hash);
    return -1;
  }

  dDebug("node:%s, proc is initialized, cqueue:%p pqueue:%p", proc->name, proc->cqueue, proc->pqueue);
  return 0;
}

static void *dmConsumChildQueue(void *param) {
  SProc        *proc = param;
  SMgmtWrapper *pWrapper = proc->wrapper;
  SProcQueue   *queue = proc->cqueue;
  void         *pHead = NULL;
  void         *pBody = NULL;
  int16_t       headLen = 0;
  int32_t       bodyLen = 0;
  int32_t       numOfMsgs = 0;
  int32_t       code = 0;
  EProcFuncType ftype = PROC_FUNC_REQ;
  SNodeMsg     *pReq = NULL;

  dDebug("node:%s, start to consume from child queue", proc->name);
  do {
    numOfMsgs = dmPopFromProcQueue(queue, &pHead, &headLen, &pBody, &bodyLen, &ftype);
    if (numOfMsgs == 0) {
      dDebug("node:%s, get no msg from child queue and exit thread", proc->name);
      break;
    }

    if (numOfMsgs < 0) {
      dError("node:%s, get no msg from child queue since %s", proc->name, terrstr());
      taosMsleep(1);
      continue;
    }

    if (ftype != PROC_FUNC_REQ) {
      dFatal("node:%s, msg:%p from child queue, invalid ftype:%d", proc->name, pHead, ftype);
      taosFreeQitem(pHead);
      rpcFreeCont(pBody);
    } else {
      dTrace("node:%s, msg:%p from child queue", proc->name, pHead);
      pReq = pHead;
      pReq->rpcMsg.pCont = pBody;
      code = dmProcessNodeMsg(pWrapper, pReq);
      if (code != 0) {
        dError("node:%s, failed to process msg:%p since %s, put into parent queue", proc->name, pReq, terrstr());
        SRpcMsg rspMsg = {
            .handle = pReq->rpcMsg.handle,
            .ahandle = pReq->rpcMsg.ahandle,
            .refId = pReq->rpcMsg.refId,
            .pCont = pReq->pRsp,
            .contLen = pReq->rspLen,
        };
        dmPutToProcPQueue(proc, &rspMsg, sizeof(SRpcMsg), rspMsg.pCont, rspMsg.contLen, PROC_FUNC_RSP);
        taosFreeQitem(pHead);
        rpcFreeCont(pBody);
        rpcFreeCont(rspMsg.pCont);
      }
    }
  } while (1);

  return NULL;
}

static void *dmConsumParentQueue(void *param) {
  SProc        *proc = param;
  SMgmtWrapper *pWrapper = proc->wrapper;
  SProcQueue   *queue = proc->pqueue;
  void         *pHead = NULL;
  void         *pBody = NULL;
  int16_t       headLen = 0;
  int32_t       bodyLen = 0;
  int32_t       numOfMsgs = 0;
  int32_t       code = 0;
  EProcFuncType ftype = PROC_FUNC_REQ;
  SRpcMsg      *pRsp = NULL;

  dDebug("node:%s, start to consume from parent queue", proc->name);
  do {
    numOfMsgs = dmPopFromProcQueue(queue, &pHead, &headLen, &pBody, &bodyLen, &ftype);
    if (numOfMsgs == 0) {
      dDebug("node:%s, get no msg from parent queue and exit thread", proc->name);
      break;
    }

    if (numOfMsgs < 0) {
      dError("node:%s, get no msg from parent queue since %s", proc->name, terrstr());
      taosMsleep(1);
      continue;
    }

    if (ftype == PROC_FUNC_RSP) {
      pRsp = pHead;
      pRsp->pCont = pBody;
      dTrace("node:%s, rsp msg:%p from parent queue, code:0x%04x handle:%p", proc->name, pRsp, code, pRsp->handle);
      dmRemoveProcRpcHandle(proc, pRsp->handle);
      rpcSendResponse(pRsp);
    } else if (ftype == PROC_FUNC_REGIST) {
      pRsp = pHead;
      dTrace("node:%s, regist msg:%p from parent queue, code:0x%04x handle:%p", proc->name, pRsp, code, pRsp->handle);
      rpcRegisterBrokenLinkArg(pRsp);
      rpcFreeCont(pBody);
    } else if (ftype == PROC_FUNC_RELEASE) {
      pRsp = pHead;
      dTrace("node:%s, release msg:%p from parent queue, code:0x%04x handle:%p", proc->name, pRsp, code, pRsp->handle);
      dmRemoveProcRpcHandle(proc, pRsp->handle);
      rpcReleaseHandle(pRsp->handle, (int8_t)pRsp->code);
      rpcFreeCont(pBody);
    } else {
      dFatal("node:%s, msg:%p get from parent queue, invalid ftype:%d", proc->name, pHead, ftype);
      rpcFreeCont(pBody);
    }

    taosFreeQitem(pHead);
  } while (1);

  return NULL;
}

int32_t dmRunProc(SProc *proc) {
  TdThreadAttr thAttr = {0};
  taosThreadAttrInit(&thAttr);
  taosThreadAttrSetDetachState(&thAttr, PTHREAD_CREATE_JOINABLE);

  if (InParentProc(proc->ptype)) {
    if (taosThreadCreate(&proc->pthread, &thAttr, dmConsumParentQueue, proc) != 0) {
      terrno = TAOS_SYSTEM_ERROR(errno);
      dError("node:%s, failed to create pthread since %s", proc->name, terrstr());
      return -1;
    }
    dDebug("node:%s, thread:%" PRId64 " is created to consume pqueue", proc->name, proc->pthread);
  }

  if (InChildProc(proc->ptype)) {
    if (taosThreadCreate(&proc->cthread, &thAttr, dmConsumChildQueue, proc) != 0) {
      terrno = TAOS_SYSTEM_ERROR(errno);
      dError("node:%s, failed to create cthread since %s", proc->name, terrstr());
      return -1;
    }
    dDebug("node:%s, thread:%" PRId64 " is created to consume cqueue", proc->name, proc->cthread);
  }

  taosThreadAttrDestroy(&thAttr);
  return 0;
}

void dmStopProc(SProc *proc) {
  if (taosCheckPthreadValid(proc->pthread)) {
    dDebug("node:%s, start to join pthread:%" PRId64, proc->name, proc->pthread);
    tsem_post(&proc->cqueue->sem);
    taosThreadJoin(proc->pthread, NULL);
    taosThreadClear(&proc->pthread);
  }

  if (taosCheckPthreadValid(proc->cthread)) {
    dDebug("node:%s, start to join cthread:%" PRId64, proc->name, proc->cthread);
    tsem_post(&proc->pqueue->sem);
    taosThreadJoin(proc->cthread, NULL);
    taosThreadClear(&proc->cthread);
  }
}

void dmCleanupProc(struct SMgmtWrapper *pWrapper) {
  SProc *proc = &pWrapper->proc;

  dDebug("node:%s, start to clean up proc", pWrapper->name);
  dmStopProc(proc);
  dmCleanupProcQueue(proc->cqueue);
  dmCleanupProcQueue(proc->pqueue);
  taosHashCleanup(proc->hash);
  dDebug("node:%s, proc is cleaned up", pWrapper->name);
}

int64_t dmRemoveProcRpcHandle(SProc *proc, void *handle) {
  int64_t h = (int64_t)handle;
  taosThreadMutexLock(&proc->cqueue->mutex);

  int64_t *pRef = taosHashGet(proc->hash, &h, sizeof(int64_t));
  int64_t  ref = 0;
  if (pRef != NULL) {
    ref = *pRef;
  }

  taosHashRemove(proc->hash, &h, sizeof(int64_t));
  taosThreadMutexUnlock(&proc->cqueue->mutex);

  return ref;
}

void dmCloseProcRpcHandles(SProc *proc) {
  taosThreadMutexLock(&proc->cqueue->mutex);
  void *h = taosHashIterate(proc->hash, NULL);
  while (h != NULL) {
    void *handle = *((void **)h);
    h = taosHashIterate(proc->hash, h);

    dError("node:%s, the child process dies and send an offline rsp to handle:%p", proc->name, handle);
    SRpcMsg rpcMsg = {.handle = handle, .code = TSDB_CODE_NODE_OFFLINE};
    rpcSendResponse(&rpcMsg);
  }
  taosHashClear(proc->hash);
  taosThreadMutexUnlock(&proc->cqueue->mutex);
}

void dmPutToProcPQueue(SProc *proc, const void *pHead, int16_t headLen, const void *pBody, int32_t bodyLen,
                       EProcFuncType ftype) {
  int32_t retry = 0;
  while (dmPushToProcQueue(proc, proc->pqueue, pHead, headLen, pBody, bodyLen, 0, 0, ftype) != 0) {
    dWarn("node:%s, failed to put msg:%p to parent queue since %s, retry:%d", proc->name, pHead, terrstr(), retry);
    retry++;
    taosMsleep(retry);
  }
}

int32_t dmPutToProcCQueue(SProc *proc, const void *pHead, int16_t headLen, const void *pBody, int32_t bodyLen,
                          void *handle, int64_t ref, EProcFuncType ftype) {
  return dmPushToProcQueue(proc, proc->cqueue, pHead, headLen, pBody, bodyLen, (int64_t)handle, ref, ftype);
}