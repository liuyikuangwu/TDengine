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

#include "streamInt.h"
#include "ttimer.h"

static void streamTaskResumeHelper(void* param, void* tmrId);
static void streamTaskSchedHelper(void* param, void* tmrId);

void streamSetupScheduleTrigger(SStreamTask* pTask) {
  int64_t delaySchema = pTask->info.delaySchedParam;
  if (delaySchema != 0 && pTask->info.fillHistory == 0) {
    int32_t ref = atomic_add_fetch_32(&pTask->refCnt, 1);
    stDebug("s-task:%s setup scheduler trigger, ref:%d delay:%" PRId64 " ms", pTask->id.idStr, ref,
            pTask->info.delaySchedParam);

    streamTmrStart(streamTaskSchedHelper, (int32_t)delaySchema, pTask, streamTimer, &pTask->schedInfo.pDelayTimer,
                   pTask->pMeta->vgId, "sched-tmr");
    pTask->schedInfo.status = TASK_TRIGGER_STATUS__INACTIVE;
  }
}

int32_t streamTrySchedExec(SStreamTask* pTask) {
  if (streamTaskSetSchedStatusWait(pTask)) {
    return streamTaskSchedTask(pTask->pMsgCb, pTask->info.nodeId, pTask->id.streamId, pTask->id.taskId, 0);
  } else {
    stTrace("s-task:%s not launch task since sched status:%d", pTask->id.idStr, pTask->status.schedStatus);
  }

  return 0;
}

int32_t streamTaskSchedTask(SMsgCb* pMsgCb, int32_t vgId, int64_t streamId, int32_t taskId, int32_t execType) {
  SStreamTaskRunReq* pRunReq = rpcMallocCont(sizeof(SStreamTaskRunReq));
  if (pRunReq == NULL) {
    stError("vgId:%d failed to create msg to start stream task:0x%x exec, type:%d, code:%s", vgId, taskId, execType,
            tstrerror(terrno));
    return terrno;
  }

  if (streamId != 0) {
    stDebug("vgId:%d create msg to for task:0x%x, exec type:%d, %s", vgId, taskId, execType,
            streamTaskGetExecType(execType));
  } else {
    stDebug("vgId:%d create msg to exec, type:%d, %s", vgId, execType, streamTaskGetExecType(execType));
  }

  pRunReq->head.vgId = vgId;
  pRunReq->streamId = streamId;
  pRunReq->taskId = taskId;
  pRunReq->reqType = execType;

  SRpcMsg msg = {.msgType = TDMT_STREAM_TASK_RUN, .pCont = pRunReq, .contLen = sizeof(SStreamTaskRunReq)};
  return tmsgPutToQueue(pMsgCb, STREAM_QUEUE, &msg);
}

void streamTaskClearSchedIdleInfo(SStreamTask* pTask) { pTask->status.schedIdleTime = 0; }

void streamTaskSetIdleInfo(SStreamTask* pTask, int32_t idleTime) { pTask->status.schedIdleTime = idleTime; }

void streamTaskResumeInFuture(SStreamTask* pTask) {
  int32_t ref = atomic_add_fetch_32(&pTask->status.timerActive, 1);
  stDebug("s-task:%s task should idle, add into timer to retry in %dms, ref:%d", pTask->id.idStr,
          pTask->status.schedIdleTime, ref);

  // add one ref count for task
  streamMetaAcquireOneTask(pTask);
  streamTmrStart(streamTaskResumeHelper, pTask->status.schedIdleTime, pTask, streamTimer, &pTask->schedInfo.pIdleTimer,
                 pTask->pMeta->vgId, "resume-task-tmr");
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void streamTaskResumeHelper(void* param, void* tmrId) {
  SStreamTask*     pTask = (SStreamTask*)param;
  SStreamTaskId*   pId = &pTask->id;
  SStreamTaskState p = streamTaskGetStatus(pTask);
  int32_t          code = 0;

  if (p.state == TASK_STATUS__DROPPING || p.state == TASK_STATUS__STOP) {
    int8_t status = streamTaskSetSchedStatusInactive(pTask);
    TAOS_UNUSED(status);

    int32_t ref = atomic_sub_fetch_32(&pTask->status.timerActive, 1);
    stDebug("s-task:%s status:%s not resume task, ref:%d", pId->idStr, p.name, ref);

    streamMetaReleaseTask(pTask->pMeta, pTask);
    return;
  }

  code = streamTaskSchedTask(pTask->pMsgCb, pTask->info.nodeId, pId->streamId, pId->taskId, STREAM_EXEC_T_RESUME_TASK);
  int32_t ref = atomic_sub_fetch_32(&pTask->status.timerActive, 1);
  if (code) {
    stError("s-task:%s sched task failed, code:%s, ref:%d", pId->idStr, tstrerror(code), ref);
  } else {
    stDebug("trigger to resume s-task:%s after idled for %dms, ref:%d", pId->idStr, pTask->status.schedIdleTime, ref);

    // release the task ref count
    streamTaskClearSchedIdleInfo(pTask);
    streamMetaReleaseTask(pTask->pMeta, pTask);
  }
}

void streamTaskSchedHelper(void* param, void* tmrId) {
  SStreamTask* pTask = (void*)param;
  const char*  id = pTask->id.idStr;
  int32_t      nextTrigger = (int32_t)pTask->info.delaySchedParam;
  int32_t      vgId = pTask->pMeta->vgId;

  int8_t status = atomic_load_8(&pTask->schedInfo.status);
  stTrace("s-task:%s in scheduler, trigger status:%d, next:%dms", id, status, nextTrigger);

  if (streamTaskShouldStop(pTask) || streamTaskShouldPause(pTask)) {
    stDebug("s-task:%s should stop, jump out of schedTimer", id);
    return;
  }

  if (streamTaskGetStatus(pTask).state == TASK_STATUS__CK) {
    stDebug("s-task:%s in checkpoint procedure, not retrieve result, next:%dms", id, nextTrigger);
  } else {
    if (status == TASK_TRIGGER_STATUS__ACTIVE) {
      SStreamTrigger* pTrigger;

      int32_t code = taosAllocateQitem(sizeof(SStreamTrigger), DEF_QITEM, 0, (void**)&pTrigger);
      if (code) {
        stError("s-task:%s failed to prepare retrieve data trigger, code:%s, try again in %dms", id, "out of memory",
                nextTrigger);
        terrno = code;
        goto _end;
      }

      pTrigger->type = STREAM_INPUT__GET_RES;
      pTrigger->pBlock = taosMemoryCalloc(1, sizeof(SSDataBlock));
      if (pTrigger->pBlock == NULL) {
        taosFreeQitem(pTrigger);

        stError("s-task:%s failed to build retrieve data trigger, code:out of memory, try again in %dms", id,
                nextTrigger);
        goto _end;
      }

      atomic_store_8(&pTask->schedInfo.status, TASK_TRIGGER_STATUS__INACTIVE);
      pTrigger->pBlock->info.type = STREAM_GET_ALL;

      code = streamTaskPutDataIntoInputQ(pTask, (SStreamQueueItem*)pTrigger);
      if (code != TSDB_CODE_SUCCESS) {
        stError("s-task:%s failed to put retrieve block into trigger, code:%s", pTask->id.idStr, tstrerror(code));
        goto _end;
      }

      code = streamTrySchedExec(pTask);
      if (code != TSDB_CODE_SUCCESS) {
        stError("s-task:%s failed to sched to run, wait for next time", pTask->id.idStr);
      }
    }
  }

_end:
  streamTmrStart(streamTaskSchedHelper, nextTrigger, pTask, streamTimer, &pTask->schedInfo.pDelayTimer, vgId,
                 "sched-run-tmr");
}
