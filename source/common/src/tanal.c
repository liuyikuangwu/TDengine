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
#include "tanal.h"
#include <curl/curl.h>
#include "tmsg.h"
#include "ttypes.h"
#include "tutil.h"

#define ANAL_ALGO_SPLIT  ","

typedef struct {
  int64_t       ver;
  SHashObj     *hash;  // algoname:algotype -> SAnalUrl
  TdThreadMutex lock;
} SAlgoMgmt;

typedef struct {
  char   *data;
  int64_t dataLen;
} SCurlResp;

static SAlgoMgmt tsAlgos = {0};
static int32_t   taosCurlTestStr(const char *url, SCurlResp *pRsp);
static int32_t   taosAnalBufGetCont(SAnalBuf *pBuf, char **ppCont, int64_t *pContLen);

const char *taosAnalAlgoStr(EAnalAlgoType type) {
  switch (type) {
    case ANAL_ALGO_TYPE_ANOMALY_DETECT:
      return "anomaly-detection";
    case ANAL_ALGO_TYPE_FORECAST:
      return "forecast";
    default:
      return "unknown";
  }
}

const char *taosAnalAlgoUrlStr(EAnalAlgoType type) {
  switch (type) {
    case ANAL_ALGO_TYPE_ANOMALY_DETECT:
      return "anomaly-detect";
    case ANAL_ALGO_TYPE_FORECAST:
      return "forecast";
    default:
      return "unknown";
  }
}

EAnalAlgoType taosAnalAlgoInt(const char *name) {
  for (EAnalAlgoType i = 0; i < ANAL_ALGO_TYPE_END; ++i) {
    if (strcasecmp(name, taosAnalAlgoStr(i)) == 0) {
      return i;
    }
  }

  return ANAL_ALGO_TYPE_END;
}

int32_t taosAnalInit() {
  if (curl_global_init(CURL_GLOBAL_ALL) != 0) {
    uError("failed to init curl env");
    return -1;
  }

  tsAlgos.ver = 0;
  taosThreadMutexInit(&tsAlgos.lock, NULL);
  tsAlgos.hash = taosHashInit(64, MurmurHash3_32, true, HASH_ENTRY_LOCK);
  if (tsAlgos.hash == NULL) {
    uError("failed to init algo hash");
    return -1;
  }

  uInfo("analysis env is initialized");
  return 0;
}

static void taosAnalFreeHash(SHashObj *hash) {
  void *pIter = taosHashIterate(hash, NULL);
  while (pIter != NULL) {
    SAnalUrl *pUrl = (SAnalUrl *)pIter;
    taosMemoryFree(pUrl->url);
    pIter = taosHashIterate(hash, pIter);
  }
  taosHashCleanup(hash);
}

void taosAnalCleanup() {
  curl_global_cleanup();
  taosThreadMutexDestroy(&tsAlgos.lock);
  taosAnalFreeHash(tsAlgos.hash);
  tsAlgos.hash = NULL;
  uInfo("analysis env is cleaned up");
}

void taosAnalUpdate(int64_t newVer, SHashObj *pHash) {
  if (newVer > tsAlgos.ver) {
    taosThreadMutexLock(&tsAlgos.lock);
    SHashObj *hash = tsAlgos.hash;
    tsAlgos.ver = newVer;
    tsAlgos.hash = pHash;
    taosThreadMutexUnlock(&tsAlgos.lock);
    taosAnalFreeHash(hash);
  } else {
    taosAnalFreeHash(pHash);
  }
}

bool taosAnalGetParaStr(const char *option, const char *paraName, char *paraValue, int32_t paraValueMaxLen) {
  char    buf[TSDB_ANAL_ALGO_OPTION_LEN] = {0};
  int32_t bufLen = snprintf(buf, sizeof(buf), "%s=", paraName);

  char *pos1 = strstr(option, buf);
  char *pos2 = strstr(option, ANAL_ALGO_SPLIT);
  if (pos1 != NULL) {
    if (paraValueMaxLen > 0) {
      int32_t copyLen = paraValueMaxLen;
      if (pos2 != NULL) {
        copyLen = (int32_t)(pos2 - pos1 - strlen(paraName) + 1);
        copyLen = MIN(copyLen, paraValueMaxLen);
      }
      tstrncpy(paraValue, pos1 + bufLen, copyLen);
    }
    return true;
  } else {
    return false;
  }
}

bool taosAnalGetParaInt(const char *option, const char *paraName, int32_t *paraValue) {
  char    buf[TSDB_ANAL_ALGO_OPTION_LEN] = {0};
  int32_t bufLen = snprintf(buf, sizeof(buf), "%s=", paraName);

  char *pos1 = strstr(option, buf);
  char *pos2 = strstr(option, ANAL_ALGO_SPLIT);
  if (pos1 != NULL) {
    *paraValue = taosStr2Int32(pos1 + bufLen + 1, NULL, 10);
    return true;
  } else {
    return false;
  }
}

int32_t taosAnalGetAlgoUrl(const char *algoName, EAnalAlgoType type, char *url, int32_t urlLen) {
  int32_t code = 0;
  char    name[TSDB_ANAL_ALGO_KEY_LEN] = {0};
  int32_t nameLen = snprintf(name, sizeof(name) - 1, "%s:%d", algoName, type);

  taosThreadMutexLock(&tsAlgos.lock);
  SAnalUrl *pUrl = taosHashAcquire(tsAlgos.hash, name, nameLen);
  if (pUrl != NULL) {
    tstrncpy(url, pUrl->url, urlLen);
    uInfo("algo:%s, type:%s, url:%s", algoName, taosAnalAlgoStr(type), url);
  } else {
    url[0] = 0;
    terrno = TSDB_CODE_ANAL_ALGO_NOT_FOUND;
    code = terrno;
    uInfo("algo:%s, type:%s, url not found", algoName, taosAnalAlgoStr(type));
  }
  taosThreadMutexUnlock(&tsAlgos.lock);

  return code;
}

int64_t taosAnalGetVersion() { return tsAlgos.ver; }

static size_t taosCurlWriteData(char *pCont, size_t contLen, size_t nmemb, void *userdata) {
  SCurlResp *pRsp = userdata;
  pRsp->dataLen = (int64_t)contLen * (int64_t)nmemb;
  pRsp->data = taosMemoryMalloc(pRsp->dataLen + 1);

  if (pRsp->data != NULL) {
    (void)memcpy(pRsp->data, pCont, pRsp->dataLen);
    pRsp->data[pRsp->dataLen] = 0;
    uInfo("curl resp is received, len:%" PRId64 ", cont:%s", pRsp->dataLen, pRsp->data);
    return pRsp->dataLen;
  } else {
    pRsp->dataLen = 0;
    uInfo("failed to malloc curl resp");
    return 0;
  }
}

static int32_t taosCurlGetRequest(const char *url, SCurlResp *pRsp) {
#if 1
  return taosCurlTestStr(url, pRsp);
#else
  CURL    *curl = NULL;
  CURLcode code = 0;

  curl = curl_easy_init();
  if (curl == NULL) {
    uError("failed to create curl handle");
    return -1;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, taosCurlWriteData);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, pRsp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 100);

  code = curl_easy_perform(curl);
  if (code != CURLE_OK) {
    uError("failed to perform curl action, code:%d", code);
  }

_OVER:
  if (curl != NULL) curl_easy_cleanup(curl);
  return code;
#endif
}

static int32_t taosCurlPostRequest(const char *url, SCurlResp *pRsp, const char *buf, int32_t bufLen) {
#if 1
  return taosCurlTestStr(url, pRsp);
#else
  struct curl_slist *headers = NULL;
  CURL              *curl = NULL;
  CURLcode           code = 0;

  curl = curl_easy_init();
  if (curl == NULL) {
    uError("failed to create curl handle");
    return -1;
  }

  headers = curl_slist_append(headers, "Content-Type:application/json;charset=UTF-8");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, taosCurlWriteData);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, pRsp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 60000);
  curl_easy_setopt(curl, CURLOPT_POST, 1);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, bufLen);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buf);

  code = curl_easy_perform(curl);
  if (code != CURLE_OK) {
    uError("failed to perform curl action, code:%d", code);
  }

_OVER:
  if (curl != NULL) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
  }
  return code;
#endif
}

SJson *taosAnalSendReqRetJson(const char *url, EAnalHttpType type, SAnalBuf *pBuf) {
  int32_t   code = -1;
  char     *pCont = NULL;
  int64_t   contentLen;
  SJson    *pJson = NULL;
  SCurlResp curlRsp = {0};

  if (type == ANAL_HTTP_TYPE_GET) {
    if (taosCurlGetRequest(url, &curlRsp) != 0) {
      terrno = TSDB_CODE_ANAL_URL_CANT_ACCESS;
      goto _OVER;
    }
  } else {
    code = taosAnalBufGetCont(pBuf, &pCont, &contentLen);
    if (code != 0) {
      terrno = code;
      goto _OVER;
    }
    if (taosCurlPostRequest(url, &curlRsp, pCont, contentLen) != 0) {
      terrno = TSDB_CODE_ANAL_URL_CANT_ACCESS;
      goto _OVER;
    }
  }

  if (curlRsp.data == NULL || curlRsp.dataLen == 0) {
    terrno = TSDB_CODE_ANAL_URL_RSP_IS_NULL;
    goto _OVER;
  }

  pJson = tjsonParse(curlRsp.data);
  if (pJson == NULL) {
    terrno = TSDB_CODE_INVALID_JSON_FORMAT;
    goto _OVER;
  }

_OVER:
  if (curlRsp.data != NULL) taosMemoryFreeClear(curlRsp.data);
  if (pCont != NULL) taosMemoryFree(pCont);
  return pJson;
}

static int32_t taosCurlTestStr(const char *url, SCurlResp *pRsp) {
  const char *listStr =
      "{\n"
      "  \"details\": [\n"
      "    {\n"
      "      \"algo\": [\n"
      "        {\n"
      "          \"name\": \"arima\"\n"
      "        },\n"
      "        {\n"
      "          \"name\": \"holt-winters\"\n"
      "        }\n"
      "      ],\n"
      "      \"type\": \"forecast\"\n"
      "    },\n"
      "    {\n"
      "      \"algo\": [\n"
      "        {\n"
      "          \"name\": \"k-sigma\"\n"
      "        },\n"
      "        {\n"
      "          \"name\": \"iqr\"\n"
      "        },\n"
      "        {\n"
      "          \"name\": \"grubbs\"\n"
      "        },\n"
      "        {\n"
      "          \"name\": \"lof\"\n"
      "        },\n"
      "        {\n"
      "          \"name\": \"esd\"\n"
      "        }\n"
      "      ],\n"
      "      \"type\": \"anomaly-detection\"\n"
      "    }\n"
      "  ],\n"
      "  \"protocol\": 0.1,\n"
      "  \"version\": 0.1\n"
      "}\n";

  const char *statusStr =
      "{"
      "  \"protocol\": 0.1,"
      "  \"status\": \"ready\""
      "}";

  const char *anomalyWindowStr =
      "{\n"
      "    \"rows\": 1,\n"
      "    \"res\": [\n"
      "        [1577808000000, 1578153600000],\n"
      "        [1578153600000, 1578240000000],\n"
      "        [1578240000000, 1578499200000]\n"
      // "        [1577808016000, 1577808016000]\n"
      "    ]\n"
      "}";

  if (strstr(url, "list") != NULL) {
    pRsp->dataLen = strlen(listStr);
    pRsp->data = taosMemoryCalloc(1, pRsp->dataLen + 1);
    strcpy(pRsp->data, listStr);
  } else if (strstr(url, "status") != NULL) {
    pRsp->dataLen = strlen(statusStr);
    pRsp->data = taosMemoryCalloc(1, pRsp->dataLen + 1);
    strcpy(pRsp->data, statusStr);
  } else if (strstr(url, "anomaly-detect") != NULL) {
    pRsp->dataLen = strlen(anomalyWindowStr);
    pRsp->data = taosMemoryCalloc(1, pRsp->dataLen + 1);
    strcpy(pRsp->data, anomalyWindowStr);
  } else {
  }

  return 0;
}

static int32_t tsosAnalJsonBufOpen(SAnalBuf *pBuf) {
  pBuf->filePtr = taosOpenFile(pBuf->fileName, TD_FILE_CREATE | TD_FILE_WRITE | TD_FILE_TRUNC | TD_FILE_WRITE_THROUGH);
  if (pBuf->filePtr == NULL) {
    return terrno;
  }
  return 0;
}

static int32_t taosAnalJsonBufWritePara(SAnalBuf *pBuf, const char *algo, const char *opt, const char *prec,
                                        int32_t col1, int32_t col2) {
  const char *js =
      "{\n"
      "\"algo\": \"%s\",\n"
      "\"opt\": \"%s\",\n"
      "\"prec\": \"%s\",\n"
      "\"schema\": [\n"
      "  [\"ts\", \"%s\", %d],\n"
      "  [\"val\", \"%s\", %d]\n"
      "],\n"
      "\"data\": [\n";
  char    buf[512] = {0};
  int32_t bufLen = snprintf(buf, sizeof(buf), js, algo, opt, prec, tDataTypes[col1].name, tDataTypes[col1].bytes,
                            tDataTypes[col2].name, tDataTypes[col2].bytes);

  if (taosWriteFile(pBuf->filePtr, buf, bufLen) != bufLen) {
    return terrno;
  }
  return 0;
}

int32_t taosAnalJsonBufNewCol(SAnalBuf *pBuf) {
  if (taosWriteFile(pBuf->filePtr, "[", 1) != 1) {
    return terrno;
  }
  return 0;
}

int32_t taosAnalJsonBufEndCol(SAnalBuf *pBuf, bool lastCol) {
  if (lastCol) {
    if (taosWriteFile(pBuf->filePtr, "]", 1) != 2) {
      return terrno;
    }
  } else {
    if (taosWriteFile(pBuf->filePtr, "],", 2) != 2) {
      return terrno;
    }
  }

  return 0;
}

static int32_t taosAnalJsonBufWriteRow(SAnalBuf *pBuf, const char *data, bool lastRow) {
  const char *js = "%s%s\n";
  char        buf[86] = {0};
  int32_t     bufLen = snprintf(buf, sizeof(buf), js, data, lastRow ? "" : ",");

  if (taosWriteFile(pBuf->filePtr, buf, bufLen) != bufLen) {
    return terrno;
  }
  return 0;
}

static int32_t taosAnalJsonBufWriteRows(SAnalBuf *pBuf, int32_t numOfRows) {
  int32_t code = 0;

  const char *js =
      "],\n"
      "\"rows\": %d\n"
      "}";
  char    buf[256] = {0};
  int32_t bufLen = snprintf(buf, sizeof(buf), js, numOfRows);

  if (taosWriteFile(pBuf->filePtr, buf, bufLen) != bufLen) {
    code = terrno;
    goto _OVER;
  }

  code = taosFsyncFile(pBuf->filePtr);
  if (code != 0) goto _OVER;

_OVER:
  (void)taosCloseFile(&pBuf->filePtr);
  return code;
}

void taosAnalJsonBufClose(SAnalBuf *pBuf) {
  if (pBuf->bufType == ANAL_BUF_TYPE_JSON) {
    if (pBuf->filePtr != NULL) {
      (void)taosCloseFile(&pBuf->filePtr);
    }
#if 0
    if (pBuf->fileName[0] != 0) {
      taosRemoveFile(pBuf->fileName);
      pBuf->fileName[0] = 0;
    }
#endif
  }
}

static int32_t taosAnalBufGetJsonCont(SAnalBuf *pBuf, char **ppCont, int64_t *pContLen) {
  int32_t   code = 0;
  int64_t   contLen;
  char     *pCont = NULL;
  TdFilePtr pFile = NULL;

  pFile = taosOpenFile(pBuf->fileName, TD_FILE_READ);
  if (pFile == NULL) {
    code = terrno;
    goto _OVER;
  }

  code = taosFStatFile(pFile, &contLen, NULL);
  if (code != 0) goto _OVER;

  pCont = taosMemoryMalloc(contLen + 1);
  if (pCont == NULL) {
    code = TSDB_CODE_OUT_OF_MEMORY;
    goto _OVER;
  }

  if (taosReadFile(pFile, pCont, contLen) != contLen) {
    code = terrno;
    goto _OVER;
  }

  pCont[contLen] = '\0';

_OVER:
  if (code == 0) {
    *ppCont = pCont;
    *pContLen = contLen;
  } else {
    if (pCont != NULL) taosMemoryFree(pCont);
  }
  if (pFile != NULL) taosCloseFile(&pFile);
  return code;
}

int32_t tsosAnalBufOpen(SAnalBuf *pBuf) {
  if (pBuf->bufType == ANAL_BUF_TYPE_JSON) {
    return tsosAnalJsonBufOpen(pBuf);
  } else {
    return TSDB_CODE_ANAL_BUF_INVALID_TYPE;
  }
}

int32_t taosAnalBufWritePara(SAnalBuf *pBuf, const char *algo, const char *opt, const char *prec, int32_t col1,
                             int32_t col2) {
  if (pBuf->bufType == ANAL_BUF_TYPE_JSON) {
    return taosAnalJsonBufWritePara(pBuf, algo, opt, prec, col1, col2);
  } else {
    return TSDB_CODE_ANAL_BUF_INVALID_TYPE;
  }
}

int32_t taosAnalBufNewCol(SAnalBuf *pBuf){
    if (pBuf->bufType == ANAL_BUF_TYPE_JSON) {
    return taosAnalJsonBufNewCol(pBuf);
  } else {
    return TSDB_CODE_ANAL_BUF_INVALID_TYPE;
  }
}

int32_t taosAnalBufEndCol(SAnalBuf *pBuf, bool lastCol) {
  if (pBuf->bufType == ANAL_BUF_TYPE_JSON) {
    return taosAnalJsonBufEndCol(pBuf, lastCol);
  } else {
    return TSDB_CODE_ANAL_BUF_INVALID_TYPE;
  }
}

int32_t taosAnalBufWriteRow(SAnalBuf *pBuf, const char *data, bool isLast) {
  if (pBuf->bufType == ANAL_BUF_TYPE_JSON) {
    return taosAnalJsonBufWriteRow(pBuf, data, isLast);
  } else {
    return TSDB_CODE_ANAL_BUF_INVALID_TYPE;
  }
}

int32_t taosAnalBufWriteRows(SAnalBuf *pBuf, int32_t numOfRows) {
  if (pBuf->bufType == ANAL_BUF_TYPE_JSON) {
    return taosAnalJsonBufWriteRows(pBuf, numOfRows);
  } else {
    return TSDB_CODE_ANAL_BUF_INVALID_TYPE;
  }
}

void taosAnalBufClose(SAnalBuf *pBuf) {
  if (pBuf->bufType == ANAL_BUF_TYPE_JSON) {
    taosAnalJsonBufClose(pBuf);
  }
}

static int32_t taosAnalBufGetCont(SAnalBuf *pBuf, char **ppCont, int64_t *pContLen) {
  *ppCont = NULL;
  *pContLen = 0;

  if (pBuf->bufType == ANAL_BUF_TYPE_JSON) {
    return taosAnalBufGetJsonCont(pBuf, ppCont, pContLen);
  } else {
    return TSDB_CODE_ANAL_BUF_INVALID_TYPE;
  }
}