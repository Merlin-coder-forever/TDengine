//
// Created by shenglian on 28/02/22.
//
#include <udf.h>
#include <string.h>
#include <uv.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdbool.h>
#include <assert.h>

//TODO: when startup, set thread poll size. add it to cfg 
//TODO: udfd restart when exist or aborts
//TODO: network error processing.
//TODO: add unit test
//TODO: add lua support
void onUdfcRead(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf);

enum {
    UV_TASK_CONNECT = 0,
    UV_TASK_REQ_RSP = 1,
    UV_TASK_DISCONNECT = 2
};

typedef struct SUdfUvSession {
    int64_t severHandle;
    uv_pipe_t *udfSvcPipe;
} SUdfUvSession;

typedef struct SClientUvTaskNode {
    int8_t type;
    int errCode;

    uv_pipe_t *pipe;

    int64_t seqNum;
    uv_buf_t reqBuf;

    uv_sem_t taskSem;
    uv_buf_t rspBuf;

    struct SClientUvTaskNode *prev;
    struct SClientUvTaskNode *next;
} SClientUvTaskNode;

typedef struct SClientUdfTask {
    int8_t type;

    SUdfUvSession *session;

    int32_t errCode;

    union {
        struct {
            SUdfSetupRequest req;
            SUdfSetupResponse rsp;
        } _setup;
        struct {
            SUdfCallRequest req;
            SUdfCallResponse rsp;
        } _call;
        struct {
            SUdfTeardownRequest req;
            SUdfTeardownResponse rsp;
        } _teardown;
    };


} SClientUdfTask;

typedef struct SClientConnBuf {
    char *buf;
    int32_t len;
    int32_t cap;
    int32_t total;
} SClientConnBuf;

typedef struct SClientUvConn {
    uv_pipe_t *pipe;
    SClientUvTaskNode taskQueue;
    SClientConnBuf readBuf;
} SClientUvConn;

uv_process_t gUdfdProcess;

uv_barrier_t gUdfInitBarrier;

uv_loop_t gUdfdLoop;
uv_thread_t gUdfLoopThread;
uv_async_t gUdfLoopTaskAync;

uv_async_t gUdfLoopStopAsync;

uv_mutex_t gUdfTaskQueueMutex;
int64_t gUdfTaskSeqNum = 0;

//double circular linked list
typedef SClientUvTaskNode *SClientUvTaskQueue;
SClientUvTaskNode gUdfQueueNode;
SClientUvTaskQueue gUdfTaskQueue = &gUdfQueueNode;

//add SClientUvTaskNode task that close conn



void udfTaskQueueInit(SClientUvTaskQueue q) {
    q->next = q;
    q->prev = q;
}

bool udfTaskQueueIsEmpty(SClientUvTaskQueue q) {
    return q == q->next;
}

void udfTaskQueueInsertTail(SClientUvTaskQueue q, SClientUvTaskNode *e) {
    e->next = q;
    e->prev = q->prev;
    e->prev->next = e;
    q->prev = e;
}

void udfTaskQueueInsertTaskAtHead(SClientUvTaskQueue q, SClientUvTaskNode *e) {
    e->next = q->next;
    e->prev = q;
    q->next->prev = e;
    q->next = e;
}

void udfTaskQueueRemoveTask(SClientUvTaskNode *e) {
    e->prev->next = e->next;
    e->next->prev = e->prev;
}

void udfTaskQueueSplit(SClientUvTaskQueue q, SClientUvTaskNode *from, SClientUvTaskQueue n) {
    n->prev = q->prev;
    n->prev->next = n;
    n->next = from;
    q->prev = from->prev;
    q->prev->next = q;
    from->prev = n;
}

SClientUvTaskNode *udfTaskQueueHeadTask(SClientUvTaskQueue q) {
    return q->next;
}

SClientUvTaskNode *udfTaskQueueTailTask(SClientUvTaskQueue q) {
    return q->prev;
}

SClientUvTaskNode *udfTaskQueueNext(SClientUvTaskNode *e) {
    return e->next;
}

void udfTaskQueueMove(SClientUvTaskQueue q, SClientUvTaskQueue n) {
    if (udfTaskQueueIsEmpty(q)) {
        udfTaskQueueInit(n);
    } else {
        SClientUvTaskNode *h = udfTaskQueueHeadTask(q);
        udfTaskQueueSplit(q, h, n);
    }
}


int32_t encodeRequest(char **pBuf, int32_t *pBufLen, SUdfRequest *request) {
    debugPrint(stdout, "%s\n", "encoding request");

    int len = sizeof(SUdfRequest) - sizeof(void *);
    switch (request->type) {
        case 0: {
            SUdfSetupRequest *setup = (SUdfSetupRequest *) (request->subReq);
            len += sizeof(SUdfSetupRequest) - 1 * sizeof(char *) + setup->pathSize;
            break;
        }
        case 1: {
            SUdfCallRequest *call = (SUdfCallRequest *) (request->subReq);
            len += sizeof(SUdfCallRequest) - 2 * sizeof(char *) + call->inputBytes + call->stateBytes;
            break;
        }
        case 2: {
            SUdfTeardownRequest *teardown = (SUdfTeardownRequest *) (request->subReq);
            len += sizeof(SUdfTeardownRequest);
            break;
        }
        default:
            break;
    }

    char *bufBegin = malloc(len);
    char *buf = bufBegin;

    //skip msgLen first
    buf += sizeof(int32_t);

    *(int64_t *) buf = request->seqNum;
    buf += sizeof(int64_t);
    *(int8_t *) buf = request->type;
    buf += sizeof(int8_t);

    switch (request->type) {
        case 0: {
            SUdfSetupRequest *setup = (SUdfSetupRequest *) (request->subReq);
            memcpy(buf, setup->udfName, 16);
            buf += 16;
            *(int8_t *) buf = setup->scriptType;
            buf += sizeof(int8_t);
            *(int8_t *) buf = setup->udfType;
            buf += sizeof(int8_t);
            *(int16_t *) buf = setup->pathSize;
            buf += sizeof(int16_t);
            memcpy(buf, setup->path, setup->pathSize);
            buf += setup->pathSize;
            break;
        }

        case 1: {
            SUdfCallRequest *call = (SUdfCallRequest *) (request->subReq);
            *(int64_t *) buf = call->udfHandle;
            buf += sizeof(int64_t);
            *(int8_t *) buf = call->step;
            buf += sizeof(int8_t);
            *(int32_t *) buf = call->inputBytes;
            buf += sizeof(int32_t);
            memcpy(buf, call->input, call->inputBytes);
            buf += call->inputBytes;
            *(int32_t *) buf = call->stateBytes;
            buf += sizeof(int32_t);
            memcpy(buf, call->state, call->stateBytes);
            buf += call->stateBytes;
            break;
        }

        case 2: {
            SUdfTeardownRequest *teardown = (SUdfTeardownRequest *) (request->subReq);
            *(int64_t *) buf = teardown->udfHandle;
            buf += sizeof(int64_t);
            break;
        }
        default:
            break;
    }

    request->msgLen = buf - bufBegin;
    *(int32_t *) bufBegin = request->msgLen;
    *pBuf = bufBegin;
    *pBufLen = request->msgLen;
    return 0;
}

int32_t decodeRequest(char *bufMsg, int32_t bufLen, SUdfRequest **pRequest) {
    debugPrint(stdout, "%s\n", "decoding request");
    if (*(int32_t *) bufMsg != bufLen) {
        debugPrint(stderr, "%s\n", "dequest request error");
        return -1;
    }
    char *buf = bufMsg;
    SUdfRequest *request = malloc(sizeof(SUdfRequest));
    request->subReq = NULL;
    request->msgLen = *(int32_t *) (buf);
    buf += sizeof(int32_t);
    request->seqNum = *(int64_t *) (buf);
    buf += sizeof(int64_t);
    request->type = *(int8_t *) (buf);
    buf += sizeof(int8_t);

    switch (request->type) {
        case 0: {
            SUdfSetupRequest *setup = malloc(sizeof(SUdfSetupRequest));

            memcpy(setup->udfName, buf, 16);
            buf += 16;
            setup->scriptType = *(int8_t *) buf;
            buf += sizeof(int8_t);
            setup->udfType = *(int8_t *) buf;
            buf += sizeof(int8_t);
            setup->pathSize = *(int16_t *) buf;
            buf += sizeof(int16_t);
            setup->path = buf;
            buf += setup->pathSize;

            request->subReq = setup;
            break;
        }
        case 1: {
            SUdfCallRequest *call = malloc(sizeof(SUdfCallRequest));

            call->udfHandle = *(int64_t *) buf;
            buf += sizeof(int64_t);
            call->step = *(int8_t *) buf;
            buf += sizeof(int8_t);
            call->inputBytes = *(int32_t *) buf;
            buf += sizeof(int32_t);
            call->input = buf;
            buf += call->inputBytes;
            call->stateBytes = *(int32_t *) buf;
            buf += sizeof(int32_t);
            call->state = buf;
            buf += call->stateBytes;

            request->subReq = call;
            break;
        }

        case 2: {
            SUdfTeardownRequest *teardown = malloc(sizeof(SUdfTeardownRequest));

            teardown->udfHandle = *(int64_t *) buf;
            buf += sizeof(int64_t);

            request->subReq = teardown;
        }

    }
    if (buf - bufMsg != bufLen) {
        debugPrint(stderr, "%s\n", "decode request error");
        free(request->subReq);
        free(request);
        return -1;
    }
    *pRequest = request;
    return 0;
}

int32_t encodeResponse(char **pBuf, int32_t *pBufLen, SUdfResponse *response) {
    debugPrint(stdout, "%s\n", "encoding response");

    int32_t len = sizeof(SUdfResponse) - sizeof(void *);

    switch (response->type) {
        case 0: {
            len += sizeof(SUdfSetupResponse);
            break;
        }
        case 1: {
            SUdfCallResponse *callResp = (SUdfCallResponse *) (response->subRsp);
            len += sizeof(SUdfCallResponse) - 2 * sizeof(char *) +
                   callResp->outputBytes + callResp->newStateBytes;
            break;
        }
        case 2: {
            len += sizeof(SUdfTeardownResponse);
            break;
        }
    }

    char *bufBegin = malloc(len);
    char *buf = bufBegin;

    //skip msgLen
    buf += sizeof(int32_t);

    *(int64_t *) buf = response->seqNum;
    buf += sizeof(int64_t);
    *(int8_t *) buf = response->type;
    buf += sizeof(int8_t);
    *(int32_t *) buf = response->code;
    buf += sizeof(int32_t);


    switch (response->type) {
        case 0: {
            SUdfSetupResponse *setupResp = (SUdfSetupResponse *) (response->subRsp);
            *(int64_t *) buf = setupResp->udfHandle;
            buf += sizeof(int64_t);
            break;
        }
        case 1: {
            SUdfCallResponse *callResp = (SUdfCallResponse *) (response->subRsp);
            *(int32_t *) buf = callResp->outputBytes;
            buf += sizeof(int32_t);
            memcpy(buf, callResp->output, callResp->outputBytes);
            buf += callResp->outputBytes;

            *(int32_t *) buf = callResp->newStateBytes;
            buf += sizeof(int32_t);
            memcpy(buf, callResp->newState, callResp->newStateBytes);
            buf += callResp->newStateBytes;
            break;
        }
        case 2: {
            SUdfTeardownResponse *teardownResp = (SUdfTeardownResponse *) (response->subRsp);
            break;
        }
        default:
            break;
    }
    response->msgLen = buf - bufBegin;
    *(int32_t *) bufBegin = response->msgLen;
    *pBuf = bufBegin;
    *pBufLen = response->msgLen;
    return 0;
}

int32_t decodeResponse(char *bufMsg, int32_t bufLen, SUdfResponse **pResponse) {
    debugPrint(stdout, "%s\n", "decoding response");

    if (*(int32_t *) bufMsg != bufLen) {
        debugPrint(stderr, "%s\n", "can not decode response");
        return -1;
    }
    char *buf = bufMsg;
    SUdfResponse *rsp = malloc(sizeof(SUdfResponse));
    rsp->msgLen = *(int32_t *) buf;
    buf += sizeof(int32_t);
    rsp->seqNum = *(int64_t *) buf;
    buf += sizeof(int64_t);
    rsp->type = *(int8_t *) buf;
    buf += sizeof(int8_t);
    rsp->code = *(int32_t *) buf;
    buf += sizeof(int32_t);

    switch (rsp->type) {
        case 0: {
            SUdfSetupResponse *setupRsp = (SUdfSetupResponse *) malloc(sizeof(SUdfSetupResponse));
            setupRsp->udfHandle = *(int64_t *) buf;
            buf += sizeof(int64_t);
            rsp->subRsp = (char *) setupRsp;
            break;
        }
        case 1: {
            SUdfCallResponse *callRsp = (SUdfCallResponse *) malloc(sizeof(SUdfCallResponse));
            callRsp->outputBytes = *(int32_t *) buf;
            buf += sizeof(int32_t);

            callRsp->output = buf;
            buf += callRsp->outputBytes;

            callRsp->newStateBytes = *(int32_t *) buf;
            buf += sizeof(int32_t);

            callRsp->newState = buf;
            buf += callRsp->newStateBytes;

            rsp->subRsp = callRsp;
            break;
        }
        case 2: {
            SUdfTeardownResponse *teardownRsp = (SUdfTeardownResponse *) malloc(sizeof(SUdfTeardownResponse));
            rsp->subRsp = teardownRsp;
            break;
        }
        default:
            break;
    }
    if (buf - bufMsg != bufLen) {
        debugPrint(stderr, "%s\n", "can not decode response");
        free(rsp->subRsp);
        free(rsp);
        return -1;
    }
    *pResponse = rsp;
    return 0;
}

void onUdfdExit(uv_process_t *req, int64_t exit_status, int term_signal) {
    debugPrint(stderr, "Process exited with status %" PRId64 ", signal %d\n", exit_status, term_signal);
    uv_close((uv_handle_t *) req, NULL);
}

void onUdfcPipeClose(uv_handle_t *handle) {
    SClientUvConn *conn = handle->data;
    if (!udfTaskQueueIsEmpty(&conn->taskQueue)) {
        SClientUvTaskNode *task = udfTaskQueueHeadTask(&conn->taskQueue);
        task->errCode = 0;
        uv_sem_post(&task->taskSem);
    }

    free(conn->readBuf.buf);
    free(conn);
    free((uv_pipe_t *) handle);

}

int32_t udfcGetUvTaskResponseResult(SClientUdfTask *task, SClientUvTaskNode *uvTask) {
    debugPrint(stdout, "%s\n", "get uv task result");
    if (uvTask->type == UV_TASK_REQ_RSP) {
        if (uvTask->rspBuf.base != NULL) {
            SUdfResponse *rsp;
            decodeResponse(uvTask->rspBuf.base, uvTask->rspBuf.len, &rsp);
            task->errCode = rsp->code;

            switch (task->type) {
                case UDF_TASK_SETUP: {
                    //TODO: copy
                    task->_setup.rsp = *(SUdfSetupResponse *) (rsp->subRsp);
                    break;
                }
                case UDF_TASK_CALL: {
                    task->_call.rsp = *(SUdfCallResponse *) (rsp->subRsp);
                    //TODO: copy
                    break;
                }
                case UDF_TASK_TEARDOWN: {
                    task->_teardown.rsp = *(SUdfTeardownResponse *) (rsp->subRsp);
                    //TODO: copy or not?
                    break;
                }
                default: {
                    break;
                }
            }

            // TODO: the call buffer is setup and freed by udf invocation
            free(uvTask->rspBuf.base);
            free(rsp->subRsp);
            free(rsp);
        } else {
            task->errCode = uvTask->errCode;
        }
    } else if (uvTask->type == UV_TASK_CONNECT) {
        task->errCode = uvTask->errCode;
    } else if (uvTask->type == UV_TASK_DISCONNECT) {
        task->errCode = uvTask->errCode;
    }
    return 0;
}

void udfcAllocateBuffer(uv_handle_t *handle, size_t suggestedSize, uv_buf_t *buf) {
    debugPrint(stdout, "%s\n", "client allocate buffer to receive from pipe");
    SClientUvConn *conn = handle->data;
    SClientConnBuf *connBuf = &conn->readBuf;

    int32_t msgHeadSize = sizeof(int32_t) + sizeof(int64_t);
    if (connBuf->cap == 0) {
        connBuf->buf = malloc(msgHeadSize);
        if (connBuf->buf) {
            connBuf->len = 0;
            connBuf->cap = msgHeadSize;
            connBuf->total = -1;

            buf->base = connBuf->buf;
            buf->len = connBuf->cap;
        } else {
            //TODO: log error
            buf->base = NULL;
            buf->len = 0;
        }
    } else {
        connBuf->cap = connBuf->total > connBuf->cap ? connBuf->total : connBuf->cap;
        void *resultBuf = realloc(connBuf->buf, connBuf->cap);
        if (resultBuf) {
            connBuf->buf = resultBuf;
            buf->base = connBuf->buf + connBuf->len;
            buf->len = connBuf->cap - connBuf->len;
        } else {
            //TODO: log error free connBuf->buf
            buf->base = NULL;
            buf->len = 0;
        }
    }

    debugPrint(stdout, "\tconn buf cap - len - total : %d - %d - %d\n", connBuf->cap, connBuf->len, connBuf->total);

}

bool isUdfcUvMsgComplete(SClientConnBuf *connBuf) {
    if (connBuf->total == -1 && connBuf->len >= sizeof(int32_t)) {
        connBuf->total = *(int32_t *) (connBuf->buf);
    }
    if (connBuf->len == connBuf->cap && connBuf->total == connBuf->cap) {
        return true;
    }
    return false;
}

void udfcUvHandleRsp(SClientUvConn *conn) {
    SClientConnBuf *connBuf = &conn->readBuf;
    int64_t seqNum = *(int64_t *) (connBuf->buf + sizeof(int32_t)); // msglen int32_t then seqnum

    if (udfTaskQueueIsEmpty(&conn->taskQueue)) {
        //LOG error
        return;
    }
    bool found = false;
    SClientUvTaskNode *taskFound = NULL;
    SClientUvTaskNode *task = udfTaskQueueNext(&conn->taskQueue);
    while (task != &conn->taskQueue) {
        if (task->seqNum == seqNum) {
            if (found == false) {
                found = true;
                taskFound = task;
            } else {
                //LOG error;
                continue;
            }
        }
        task = udfTaskQueueNext(task);
    }

    if (taskFound) {
        taskFound->rspBuf = uv_buf_init(connBuf->buf, connBuf->len);
        udfTaskQueueRemoveTask(taskFound);
        uv_sem_post(&taskFound->taskSem);
    } else {
        //LOG error
    }
    connBuf->buf = NULL;
    connBuf->total = -1;
    connBuf->len = 0;
    connBuf->cap = 0;
}

void udfcUvHandleError(SClientUvConn *conn) {
    uv_close((uv_handle_t *) conn->pipe, onUdfcPipeClose);
}

void onUdfcRead(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf) {
    debugPrint(stdout, "%s, nread: %zd\n", "client read from pipe", nread);
    if (nread == 0) return;

    SClientUvConn *conn = client->data;
    SClientConnBuf *connBuf = &conn->readBuf;
    if (nread > 0) {
        connBuf->len += nread;
        if (isUdfcUvMsgComplete(connBuf)) {
            udfcUvHandleRsp(conn);
        }

    }
    if (nread < 0) {
        debugPrint(stderr, "\tclient read error: %s\n", uv_strerror(nread));
        if (nread == UV_EOF) {
            //TODO:
        }
        udfcUvHandleError(conn);
    }

}

void onUdfClientWrite(uv_write_t *write, int status) {
    debugPrint(stderr, "%s\n", "after writing to pipe");
    SClientUvTaskNode *uvTask = write->data;
    if (status == 0) {
        uv_pipe_t *pipe = uvTask->pipe;
        SClientUvConn *conn = pipe->data;
        udfTaskQueueInsertTail(&conn->taskQueue, uvTask);
    } else {
        //TODO Log error;
    }
    debugPrint(stdout, "\tlength:%zu\n", uvTask->reqBuf.len);
    free(write);
    free(uvTask->reqBuf.base);
}

void onUdfClientConnect(uv_connect_t *connect, int status) {
    SClientUvTaskNode *uvTask = connect->data;
    uvTask->errCode = status;
    if (status != 0) {
        //TODO: LOG error
    }
    uv_read_start((uv_stream_t *) uvTask->pipe, udfcAllocateBuffer, onUdfcRead);
    free(connect);
    uv_sem_post(&uvTask->taskSem);
}

int32_t createUdfcUvTask(SClientUdfTask *task, int8_t uvTaskType, SClientUvTaskNode **pUvTask) {
    SClientUvTaskNode *uvTask = calloc(1, sizeof(SClientUvTaskNode));
    uvTask->type = uvTaskType;

    if (uvTaskType == UV_TASK_CONNECT) {
    } else if (uvTaskType == UV_TASK_REQ_RSP) {
        uvTask->pipe = task->session->udfSvcPipe;
        SUdfRequest request;
        request.type = task->type;
        request.seqNum = gUdfTaskSeqNum++;

        if (task->type == UDF_TASK_SETUP) {
            request.subReq = &task->_setup.req;
            request.type = UDF_TASK_SETUP;
        } else if (task->type == UDF_TASK_CALL) {
            request.subReq = &task->_call.req;
            request.type = UDF_TASK_CALL;
        } else if (task->type == UDF_TASK_TEARDOWN) {
            request.subReq = &task->_teardown.req;
            request.type = UDF_TASK_TEARDOWN;
        } else {
            //TODO log and return error
        }
        char *buf = NULL;
        int32_t bufLen = 0;
        encodeRequest(&buf, &bufLen, &request);
        uvTask->reqBuf = uv_buf_init(buf, bufLen);
        uvTask->seqNum = request.seqNum;
    } else if (uvTaskType == UV_TASK_DISCONNECT) {
        uvTask->pipe = task->session->udfSvcPipe;
    }
    uv_sem_init(&uvTask->taskSem, 0);

    *pUvTask = uvTask;
    return 0;
}

int32_t queueUvUdfTask(SClientUvTaskNode *uvTask) {
    debugPrint(stdout, "%s, %d\n", "queue uv task", uvTask->type);

    uv_mutex_lock(&gUdfTaskQueueMutex);
    udfTaskQueueInsertTail(gUdfTaskQueue, uvTask);
    uv_mutex_unlock(&gUdfTaskQueueMutex);
    uv_async_send(&gUdfLoopTaskAync);

    uv_sem_wait(&uvTask->taskSem);
    uv_sem_destroy(&uvTask->taskSem);

    return 0;
}

int32_t startUvUdfTask(SClientUvTaskNode *uvTask) {
    debugPrint(stdout, "%s, type %d\n", "start uv task ", uvTask->type);
    switch (uvTask->type) {
        case UV_TASK_CONNECT: {
            uv_pipe_t *pipe = malloc(sizeof(uv_pipe_t));
            uv_pipe_init(&gUdfdLoop, pipe, 0);
            uvTask->pipe = pipe;

            SClientUvConn *conn = malloc(sizeof(SClientUvConn));
            conn->pipe = pipe;
            conn->readBuf.len = 0;
            conn->readBuf.cap = 0;
            conn->readBuf.buf = 0;
            conn->readBuf.total = -1;
            udfTaskQueueInit(&conn->taskQueue);

            pipe->data = conn;

            uv_connect_t *connReq = malloc(sizeof(uv_connect_t));
            connReq->data = uvTask;

            uv_pipe_connect(connReq, pipe, "udf.sock", onUdfClientConnect);
            break;
        }
        case UV_TASK_REQ_RSP: {
            uv_pipe_t *pipe = uvTask->pipe;
            uv_write_t *write = malloc(sizeof(uv_write_t));
            write->data = uvTask;
            uv_write(write, (uv_stream_t *) pipe, &uvTask->reqBuf, 1, onUdfClientWrite);
            break;
        }
        case UV_TASK_DISCONNECT: {
            SClientUvConn *conn = uvTask->pipe->data;
            udfTaskQueueInsertTail(&conn->taskQueue, uvTask);
            uv_close((uv_handle_t *) uvTask->pipe, onUdfcPipeClose);
            break;
        }
        default: {
            break;
        }
    }

    return 0;
}

void udfClientAsyncCb(uv_async_t *async) {
    SClientUvTaskNode node;
    SClientUvTaskQueue q = &node;
    udfTaskQueueInit(q);

    uv_mutex_lock(&gUdfTaskQueueMutex);
    udfTaskQueueMove(gUdfTaskQueue, q);
    uv_mutex_unlock(&gUdfTaskQueueMutex);

    while (!udfTaskQueueIsEmpty(q)) {
        SClientUvTaskNode *task = udfTaskQueueHeadTask(q);
        udfTaskQueueRemoveTask(task);
        startUvUdfTask(task);
    }

}

void udfStopAsyncCb(uv_async_t *async) {
    uv_stop(&gUdfdLoop);
    uv_loop_close(&gUdfdLoop);
}

void startUdfd(void *argsThread) {
    uv_loop_init(&gUdfdLoop);

//    uv_process_options_t options;
//    static char path[256] = {0};
//    size_t cwdSize;
//    uv_cwd(path, &cwdSize);
//    strcat(path, "/udfd");
//    char* args[2] = {path, NULL};
//    options.args = args;
//    options.file = path;
//    options.exit_cb = onUdfdExit;
//
//    int err = uv_spawn(&gUdfdLoop, &gUdfdProcess, &options);
//    if (err != 0) {
//        debugPrint(stderr, "can not spawn udfd. error: %s", uv_strerror(err));
//    }

    uv_async_init(&gUdfdLoop, &gUdfLoopTaskAync, udfClientAsyncCb);
    uv_async_init(&gUdfdLoop, &gUdfLoopStopAsync, udfStopAsyncCb);
    uv_mutex_init(&gUdfTaskQueueMutex);
    udfTaskQueueInit(gUdfTaskQueue);
    uv_barrier_wait(&gUdfInitBarrier);
    uv_run(&gUdfdLoop, UV_RUN_DEFAULT);
}

int32_t startUdfService() {
    uv_barrier_init(&gUdfInitBarrier, 2);
    uv_thread_create(&gUdfLoopThread, startUdfd, 0);
    uv_barrier_wait(&gUdfInitBarrier);
    return 0;
}

int32_t stopUdfService() {
    uv_barrier_destroy(&gUdfInitBarrier);
    //uv_process_kill(&gUdfdProcess, SIGINT);
    uv_async_send(&gUdfLoopStopAsync);
    uv_mutex_destroy(&gUdfTaskQueueMutex);
    uv_thread_join(&gUdfLoopThread);
    return 0;
}

int32_t udfcRunUvTask(SClientUdfTask *task, int8_t uvTaskType) {
    SClientUvTaskNode *uvTask = NULL;

    createUdfcUvTask(task, uvTaskType, &uvTask);
    queueUvUdfTask(uvTask);
    udfcGetUvTaskResponseResult(task, uvTask);
    if (uvTaskType == UV_TASK_CONNECT) {
        task->session->udfSvcPipe = uvTask->pipe;
    }
    free(uvTask);
    uvTask = NULL;
    return task->errCode;
}

int32_t setupUdf(SUdfInfo *udfInfo, UdfHandle *handle) {
    debugPrint(stdout, "%s\n", "client setup udf");
    SClientUdfTask *task = malloc(sizeof(SClientUdfTask));
    task->errCode = 0;
    task->session = malloc(sizeof(SUdfUvSession));
    task->type = UDF_TASK_SETUP;

    SUdfSetupRequest *req = &task->_setup.req;
    memcpy(req->udfName, udfInfo->udfName, 16);
    req->path = udfInfo->path;
    req->pathSize = strlen(req->path) + 1;
    req->udfType = 0;
    req->scriptType = 0;

    int32_t errCode = udfcRunUvTask(task, UV_TASK_CONNECT);
    if (errCode != 0) {
        //TODO: log error
        return -1;
    }

    udfcRunUvTask(task, UV_TASK_REQ_RSP);

    SUdfSetupResponse *rsp = &task->_setup.rsp;
    task->session->severHandle = rsp->udfHandle;
    *handle = task->session;
    int32_t err = task->errCode;
    free(task);
    return err;
}

int32_t callUdf(UdfHandle handle, int8_t step, char *state, int32_t stateSize, SSDataBlock input, char **newState,
                int32_t *newStateSize, SSDataBlock *output) {
    debugPrint(stdout, "%s\n", "client call udf");

    SClientUdfTask *task = malloc(sizeof(SClientUdfTask));
    task->errCode = 0;
    task->session = (SUdfUvSession *) handle;
    task->type = UDF_TASK_CALL;

    SUdfCallRequest *req = &task->_call.req;

    req->state = state;
    req->stateBytes = stateSize;
    req->inputBytes = input.size;
    req->input = input.data;
    req->udfHandle = task->session->severHandle;
    req->step = step;

    udfcRunUvTask(task, UV_TASK_REQ_RSP);

    SUdfCallResponse *rsp = &task->_call.rsp;
    *newState = rsp->newState;
    *newStateSize = rsp->newStateBytes;
    output->size = rsp->outputBytes;
    output->data = rsp->output;
    int32_t err = task->errCode;
    free(task);
    return err;
}

int32_t teardownUdf(UdfHandle handle) {
    debugPrint(stdout, "%s\n", "client teardown udf");

    SClientUdfTask *task = malloc(sizeof(SClientUdfTask));
    task->errCode = 0;
    task->session = (SUdfUvSession *) handle;
    task->type = UDF_TASK_TEARDOWN;

    SUdfTeardownRequest *req = &task->_teardown.req;
    req->udfHandle = task->session->severHandle;

    udfcRunUvTask(task, UV_TASK_REQ_RSP);


    SUdfTeardownResponse *rsp = &task->_teardown.rsp;

    int32_t err = task->errCode;

    udfcRunUvTask(task, UV_TASK_DISCONNECT);

    free(task->session);
    free(task);

    return err;
}
