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

#include "executor.h"
#include "streamInc.h"
#include "tcommon.h"
#include "ttimer.h"

SStreamState* streamStateOpen(char* path, SStreamTask* pTask, bool specPath) {
  SStreamState* pState = taosMemoryCalloc(1, sizeof(SStreamState));
  if (pState == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }

  char statePath[300];
  if (!specPath) {
    sprintf(statePath, "%s/%d", path, pTask->taskId);
  } else {
    strncpy(statePath, path, 300);
  }
  if (tdbOpen(statePath, 4096, 256, &pState->db) < 0) {
    goto _err;
  }

  // open state storage backend
  if (tdbTbOpen("state.db", sizeof(SWinKey), -1, SWinKeyCmpr, pState->db, &pState->pStateDb) < 0) {
    goto _err;
  }

  // todo refactor
  if (tdbTbOpen("func.state.db", sizeof(SWinKey), -1, SWinKeyCmpr, pState->db, &pState->pFillStateDb) < 0) {
    goto _err;
  }

  if (tdbTbOpen("func.state.db", sizeof(STupleKey), -1, STupleKeyCmpr, pState->db, &pState->pFuncStateDb) < 0) {
    goto _err;
  }

  if (streamStateBegin(pState) < 0) {
    goto _err;
  }

  pState->pOwner = pTask;

  return pState;

_err:
  tdbTbClose(pState->pStateDb);
  tdbTbClose(pState->pFuncStateDb);
  tdbTbClose(pState->pFillStateDb);
  tdbClose(pState->db);
  taosMemoryFree(pState);
  return NULL;
}

void streamStateClose(SStreamState* pState) {
  tdbCommit(pState->db, &pState->txn);
  tdbTbClose(pState->pStateDb);
  tdbTbClose(pState->pFuncStateDb);
  tdbTbClose(pState->pFillStateDb);
  tdbClose(pState->db);

  taosMemoryFree(pState);
}

int32_t streamStateBegin(SStreamState* pState) {
  if (tdbTxnOpen(&pState->txn, 0, tdbDefaultMalloc, tdbDefaultFree, NULL, TDB_TXN_WRITE | TDB_TXN_READ_UNCOMMITTED) <
      0) {
    return -1;
  }

  if (tdbBegin(pState->db, &pState->txn) < 0) {
    tdbTxnClose(&pState->txn);
    return -1;
  }
  return 0;
}

int32_t streamStateCommit(SStreamState* pState) {
  if (tdbCommit(pState->db, &pState->txn) < 0) {
    return -1;
  }
  memset(&pState->txn, 0, sizeof(TXN));
  if (tdbTxnOpen(&pState->txn, 0, tdbDefaultMalloc, tdbDefaultFree, NULL, TDB_TXN_WRITE | TDB_TXN_READ_UNCOMMITTED) <
      0) {
    return -1;
  }
  if (tdbBegin(pState->db, &pState->txn) < 0) {
    return -1;
  }
  return 0;
}

int32_t streamStateAbort(SStreamState* pState) {
  if (tdbAbort(pState->db, &pState->txn) < 0) {
    return -1;
  }
  memset(&pState->txn, 0, sizeof(TXN));
  if (tdbTxnOpen(&pState->txn, 0, tdbDefaultMalloc, tdbDefaultFree, NULL, TDB_TXN_WRITE | TDB_TXN_READ_UNCOMMITTED) <
      0) {
    return -1;
  }
  if (tdbBegin(pState->db, &pState->txn) < 0) {
    return -1;
  }
  return 0;
}

int32_t streamStateFuncPut(SStreamState* pState, const STupleKey* key, const void* value, int32_t vLen) {
  return tdbTbUpsert(pState->pFuncStateDb, key, sizeof(STupleKey), value, vLen, &pState->txn);
}
int32_t streamStateFuncGet(SStreamState* pState, const STupleKey* key, void** pVal, int32_t* pVLen) {
  return tdbTbGet(pState->pFuncStateDb, key, sizeof(STupleKey), pVal, pVLen);
}

int32_t streamStateFuncDel(SStreamState* pState, const STupleKey* key) {
  return tdbTbDelete(pState->pFuncStateDb, key, sizeof(STupleKey), &pState->txn);
}

int32_t streamStatePut(SStreamState* pState, const SWinKey* key, const void* value, int32_t vLen) {
  return tdbTbUpsert(pState->pStateDb, key, sizeof(SWinKey), value, vLen, &pState->txn);
}

// todo refactor
int32_t streamStateFillPut(SStreamState* pState, const SWinKey* key, const void* value, int32_t vLen) {
  return tdbTbUpsert(pState->pFillStateDb, key, sizeof(SWinKey), value, vLen, &pState->txn);
}

int32_t streamStateGet(SStreamState* pState, const SWinKey* key, void** pVal, int32_t* pVLen) {
  return tdbTbGet(pState->pStateDb, key, sizeof(SWinKey), pVal, pVLen);
}

// todo refactor
int32_t streamStateFillGet(SStreamState* pState, const SWinKey* key, void** pVal, int32_t* pVLen) {
  return tdbTbGet(pState->pFillStateDb, key, sizeof(SWinKey), pVal, pVLen);
}

int32_t streamStateDel(SStreamState* pState, const SWinKey* key) {
  return tdbTbDelete(pState->pStateDb, key, sizeof(SWinKey), &pState->txn);
}

// todo refactor
int32_t streamStateFillDel(SStreamState* pState, const SWinKey* key) {
  return tdbTbDelete(pState->pFillStateDb, key, sizeof(SWinKey), &pState->txn);
}

int32_t streamStateAddIfNotExist(SStreamState* pState, const SWinKey* key, void** pVal, int32_t* pVLen) {
  // todo refactor
  int32_t size = *pVLen;
  if (streamStateGet(pState, key, pVal, pVLen) == 0) {
    return 0;
  }
  *pVal = tdbRealloc(NULL, size);
  memset(*pVal, 0, size);
  return 0;
}

int32_t streamStateReleaseBuf(SStreamState* pState, const SWinKey* key, void* pVal) {
  // todo refactor
  streamFreeVal(pVal);
  return 0;
}

SStreamStateCur* streamStateGetCur(SStreamState* pState, const SWinKey* key) {
  SStreamStateCur* pCur = taosMemoryCalloc(1, sizeof(SStreamStateCur));
  if (pCur == NULL) return NULL;
  tdbTbcOpen(pState->pStateDb, &pCur->pCur, NULL);

  int32_t c;
  tdbTbcMoveTo(pCur->pCur, key, sizeof(SWinKey), &c);
  if (c != 0) {
    taosMemoryFree(pCur);
    return NULL;
  }
  return pCur;
}

SStreamStateCur* streamStateFillGetCur(SStreamState* pState, const SWinKey* key) {
  SStreamStateCur* pCur = taosMemoryCalloc(1, sizeof(SStreamStateCur));
  if (pCur == NULL) return NULL;
  tdbTbcOpen(pState->pFillStateDb, &pCur->pCur, NULL);

  int32_t c;
  tdbTbcMoveTo(pCur->pCur, key, sizeof(SWinKey), &c);
  if (c != 0) {
    taosMemoryFree(pCur);
    return NULL;
  }
  return pCur;
}

SStreamStateCur* streamStateGetAndCheckCur(SStreamState* pState, SWinKey* key) {
  SStreamStateCur* pCur = streamStateFillGetCur(pState, key);
  if (pCur) {
    int32_t code = streamStateGetGroupKVByCur(pCur, key, NULL, 0);
    if (code == 0) {
      return pCur;
    }
  }
  return NULL;
}

int32_t streamStateGetKVByCur(SStreamStateCur* pCur, SWinKey* pKey, const void** pVal, int32_t* pVLen) {
  const SWinKey* pKTmp = NULL;
  int32_t        kLen;
  if (tdbTbcGet(pCur->pCur, (const void**)&pKTmp, &kLen, pVal, pVLen) < 0) {
    return -1;
  }
  *pKey = *pKTmp;
  return 0;
}

int32_t streamStateGetGroupKVByCur(SStreamStateCur* pCur, SWinKey* pKey, const void** pVal, int32_t* pVLen) {
  uint64_t groupId = pKey->groupId;
  int32_t  code = streamStateGetKVByCur(pCur, pKey, pVal, pVLen);
  if (code == 0) {
    if (pKey->groupId == groupId) {
      return 0;
    }
  }
  return -1;
}

int32_t streamStateSeekFirst(SStreamState* pState, SStreamStateCur* pCur) {
  //
  return tdbTbcMoveToFirst(pCur->pCur);
}

int32_t streamStateSeekLast(SStreamState* pState, SStreamStateCur* pCur) {
  //
  return tdbTbcMoveToLast(pCur->pCur);
}

SStreamStateCur* streamStateFillSeekKeyNext(SStreamState* pState, const SWinKey* key) {
  SStreamStateCur* pCur = taosMemoryCalloc(1, sizeof(SStreamStateCur));
  if (pCur == NULL) {
    return NULL;
  }
  if (tdbTbcOpen(pState->pFillStateDb, &pCur->pCur, NULL) < 0) {
    taosMemoryFree(pCur);
    return NULL;
  }

  int32_t c;
  if (tdbTbcMoveTo(pCur->pCur, key, sizeof(SWinKey), &c) < 0) {
    tdbTbcClose(pCur->pCur);
    taosMemoryFree(pCur);
    return NULL;
  }
  if (c > 0) return pCur;

  if (tdbTbcMoveToNext(pCur->pCur) < 0) {
    taosMemoryFree(pCur);
    return NULL;
  }

  return pCur;
}

SStreamStateCur* streamStateFillSeekKeyPrev(SStreamState* pState, const SWinKey* key) {
  SStreamStateCur* pCur = taosMemoryCalloc(1, sizeof(SStreamStateCur));
  if (pCur == NULL) {
    return NULL;
  }
  if (tdbTbcOpen(pState->pFillStateDb, &pCur->pCur, NULL) < 0) {
    taosMemoryFree(pCur);
    return NULL;
  }

  int32_t c;
  if (tdbTbcMoveTo(pCur->pCur, key, sizeof(SWinKey), &c) < 0) {
    tdbTbcClose(pCur->pCur);
    taosMemoryFree(pCur);
    return NULL;
  }
  if (c < 0) return pCur;

  if (tdbTbcMoveToPrev(pCur->pCur) < 0) {
    taosMemoryFree(pCur);
    return NULL;
  }

  return pCur;
}

int32_t streamStateCurNext(SStreamState* pState, SStreamStateCur* pCur) {
  //
  return tdbTbcMoveToNext(pCur->pCur);
}

int32_t streamStateCurPrev(SStreamState* pState, SStreamStateCur* pCur) {
  //
  return tdbTbcMoveToPrev(pCur->pCur);
}
void streamStateFreeCur(SStreamStateCur* pCur) {
  tdbTbcClose(pCur->pCur);
  taosMemoryFree(pCur);
}

void streamFreeVal(void* val) { tdbFree(val); }
