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

#include "builtins.h"
#include "builtinsimpl.h"
#include "cJSON.h"
#include "geomFunc.h"
#include "querynodes.h"
#include "scalar.h"
#include "tanal.h"
#include "taoserror.h"
#include "ttime.h"

static int32_t buildFuncErrMsg(char* pErrBuf, int32_t len, int32_t errCode, const char* pFormat, ...) {
  va_list vArgList;
  va_start(vArgList, pFormat);
  (void)vsnprintf(pErrBuf, len, pFormat, vArgList);
  va_end(vArgList);
  return errCode;
}

static int32_t invaildFuncParaNumErrMsg(char* pErrBuf, int32_t len, const char* pFuncName) {
  return buildFuncErrMsg(pErrBuf, len, TSDB_CODE_FUNC_FUNTION_PARA_NUM, "Invalid number of parameters : %s", pFuncName);
}

static int32_t invaildFuncParaTypeErrMsg(char* pErrBuf, int32_t len, const char* pFuncName) {
  return buildFuncErrMsg(pErrBuf, len, TSDB_CODE_FUNC_FUNTION_PARA_TYPE, "Invalid parameter data type : %s", pFuncName);
}

static int32_t invaildFuncParaValueErrMsg(char* pErrBuf, int32_t len, const char* pFuncName) {
  return buildFuncErrMsg(pErrBuf, len, TSDB_CODE_FUNC_FUNTION_PARA_VALUE, "Invalid parameter value : %s", pFuncName);
}

static int32_t validateTimeUnitParam(uint8_t dbPrec, const SValueNode* pVal) {
  if (!IS_DURATION_VAL(pVal->flag)) {
    return TSDB_CODE_FUNC_TIME_UNIT_INVALID;
  }

  if (TSDB_TIME_PRECISION_MILLI == dbPrec &&
      (0 == strcasecmp(pVal->literal, "1u") || 0 == strcasecmp(pVal->literal, "1b"))) {
    return TSDB_CODE_FUNC_TIME_UNIT_TOO_SMALL;
  }

  if (TSDB_TIME_PRECISION_MICRO == dbPrec && 0 == strcasecmp(pVal->literal, "1b")) {
    return TSDB_CODE_FUNC_TIME_UNIT_TOO_SMALL;
  }

  if (pVal->literal[0] != '1' ||
      (pVal->literal[1] != 'u' && pVal->literal[1] != 'a' && pVal->literal[1] != 's' && pVal->literal[1] != 'm' &&
       pVal->literal[1] != 'h' && pVal->literal[1] != 'd' && pVal->literal[1] != 'w' && pVal->literal[1] != 'b')) {
    return TSDB_CODE_FUNC_TIME_UNIT_INVALID;
  }

  return TSDB_CODE_SUCCESS;
}

/* Following are valid ISO-8601 timezone format:
 * 1 z/Z
 * 2 ±hh:mm
 * 3 ±hhmm
 * 4 ±hh
 *
 */

static bool validateHourRange(int8_t hour) {
  if (hour < 0 || hour > 12) {
    return false;
  }

  return true;
}

static bool validateMinuteRange(int8_t hour, int8_t minute, char sign) {
  if (minute == 0 || (minute == 30 && (hour == 3 || hour == 5) && sign == '+')) {
    return true;
  }

  return false;
}

static bool validateTimezoneFormat(const SValueNode* pVal) {
  if (TSDB_DATA_TYPE_BINARY != pVal->node.resType.type) {
    return false;
  }

  char*   tz = varDataVal(pVal->datum.p);
  int32_t len = varDataLen(pVal->datum.p);

  char   buf[3] = {0};
  int8_t hour = -1, minute = -1;
  if (len == 0) {
    return false;
  } else if (len == 1 && (tz[0] == 'z' || tz[0] == 'Z')) {
    return true;
  } else if ((tz[0] == '+' || tz[0] == '-')) {
    switch (len) {
      case 3:
      case 5: {
        for (int32_t i = 1; i < len; ++i) {
          if (!isdigit(tz[i])) {
            return false;
          }

          if (i == 2) {
            (void)memcpy(buf, &tz[i - 1], 2);
            hour = taosStr2Int8(buf, NULL, 10);
            if (!validateHourRange(hour)) {
              return false;
            }
          } else if (i == 4) {
            (void)memcpy(buf, &tz[i - 1], 2);
            minute = taosStr2Int8(buf, NULL, 10);
            if (!validateMinuteRange(hour, minute, tz[0])) {
              return false;
            }
          }
        }
        break;
      }
      case 6: {
        for (int32_t i = 1; i < len; ++i) {
          if (i == 3) {
            if (tz[i] != ':') {
              return false;
            }
            continue;
          }

          if (!isdigit(tz[i])) {
            return false;
          }

          if (i == 2) {
            (void)memcpy(buf, &tz[i - 1], 2);
            hour = taosStr2Int8(buf, NULL, 10);
            if (!validateHourRange(hour)) {
              return false;
            }
          } else if (i == 5) {
            (void)memcpy(buf, &tz[i - 1], 2);
            minute = taosStr2Int8(buf, NULL, 10);
            if (!validateMinuteRange(hour, minute, tz[0])) {
              return false;
            }
          }
        }
        break;
      }
      default: {
        return false;
      }
    }
  } else {
    return false;
  }

  return true;
}

static int32_t countTrailingSpaces(const SValueNode* pVal, bool isLtrim) {
  int32_t numOfSpaces = 0;
  int32_t len = varDataLen(pVal->datum.p);
  char*   str = varDataVal(pVal->datum.p);

  int32_t startPos = isLtrim ? 0 : len - 1;
  int32_t step = isLtrim ? 1 : -1;
  for (int32_t i = startPos; i < len || i >= 0; i += step) {
    if (!isspace(str[i])) {
      break;
    }
    numOfSpaces++;
  }

  return numOfSpaces;
}

static int32_t addTimezoneParam(SNodeList* pList) {
  char      buf[TD_TIME_STR_LEN] = {0};
  time_t    t = taosTime(NULL);
  struct tm tmInfo;
  if (taosLocalTime(&t, &tmInfo, buf, sizeof(buf)) != NULL) {
    (void)strftime(buf, sizeof(buf), "%z", &tmInfo);
  }
  int32_t len = (int32_t)strlen(buf);

  SValueNode* pVal = NULL;
  int32_t code = nodesMakeNode(QUERY_NODE_VALUE, (SNode**)&pVal);
  if (pVal == NULL) {
    return code;
  }

  pVal->literal = taosStrndup(buf, len);
  if (pVal->literal == NULL) {
    nodesDestroyNode((SNode*)pVal);
    return terrno;
  }
  pVal->translate = true;
  pVal->node.resType.type = TSDB_DATA_TYPE_BINARY;
  pVal->node.resType.bytes = len + VARSTR_HEADER_SIZE;
  pVal->node.resType.precision = TSDB_TIME_PRECISION_MILLI;
  pVal->datum.p = taosMemoryCalloc(1, len + VARSTR_HEADER_SIZE + 1);
  if (pVal->datum.p == NULL) {
    nodesDestroyNode((SNode*)pVal);
    return terrno;
  }
  varDataSetLen(pVal->datum.p, len);
  tstrncpy(varDataVal(pVal->datum.p), pVal->literal, len + 1);

  code = nodesListAppend(pList, (SNode*)pVal);
  if (TSDB_CODE_SUCCESS != code) {
    nodesDestroyNode((SNode*)pVal);
    return code;
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t addUint8Param(SNodeList** pList, uint8_t param) {
  SValueNode* pVal = NULL;
  int32_t code = nodesMakeNode(QUERY_NODE_VALUE, (SNode**)&pVal);
  if (pVal == NULL) {
    return code;
  }

  pVal->literal = NULL;
  pVal->translate = true;
  pVal->notReserved = true;
  pVal->node.resType.type = TSDB_DATA_TYPE_TINYINT;
  pVal->node.resType.bytes = tDataTypes[TSDB_DATA_TYPE_TINYINT].bytes;
  pVal->node.resType.precision = param;
  pVal->datum.i = (int64_t)param;
  pVal->typeData = (int64_t)param;

  code = nodesListMakeAppend(pList, (SNode*)pVal);
  if (TSDB_CODE_SUCCESS != code) {
    nodesDestroyNode((SNode*)pVal);
    return code;
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t addPseudoParam(SNodeList** pList) {
  SNode *pseudoNode = NULL;
  int32_t code = nodesMakeNode(QUERY_NODE_LEFT_VALUE, &pseudoNode);
  if (pseudoNode == NULL) {
    return code;
  }

  code = nodesListMakeAppend(pList, pseudoNode);
  if (TSDB_CODE_SUCCESS != code) {
    nodesDestroyNode(pseudoNode);
    return code;
  }
  return TSDB_CODE_SUCCESS;
}

static SDataType* getSDataTypeFromNode(SNode* pNode) {
  if (pNode == NULL) return NULL;
  if (nodesIsExprNode(pNode)) {
    return &((SExprNode*)pNode)->resType;
  } else if (QUERY_NODE_COLUMN_REF == pNode->type) {
    return &((SColumnRefNode*)pNode)->resType;
  } else {
    return NULL;
  }
}

static bool paramSupportNull(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_NULL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE);
}

static bool paramSupportBool(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_BOOL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE);
}

static bool paramSupportTinyint(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_TINYINT_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_NUMERIC_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_INTEGER_TYPE);
}

static bool paramSupportSmallint(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_SMALLINT_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_NUMERIC_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_INTEGER_TYPE);
}

static bool paramSupportInt(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_INT_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_NUMERIC_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_INTEGER_TYPE);
}

static bool paramSupportBigint(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_BIGINT_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_NUMERIC_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_INTEGER_TYPE);
}

static bool paramSupportFloat(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_FLOAT_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_NUMERIC_TYPE);
}

static bool paramSupportDouble(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_DOUBLE_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_NUMERIC_TYPE);
}

static bool paramSupportVarchar(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_VARCHAR_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_STRING_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_VAR_TYPE);
}

static bool paramSupportTimestamp(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE);
}

static bool paramSupportNchar(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_NCHAR_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_STRING_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_VAR_TYPE);
}

static bool paramSupportUTinyInt(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_UTINYINT_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_NUMERIC_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_INTEGER_TYPE);
}

static bool paramSupportUSmallInt(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_USMALLINT_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_NUMERIC_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_INTEGER_TYPE);
}

static bool paramSupportUInt(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_UINT_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_NUMERIC_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_INTEGER_TYPE);
}

static bool paramSupportUBigInt(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_UBIGINT_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_NUMERIC_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_INTEGER_TYPE);
}

static bool paramSupportJSON(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_JSON_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_VAR_TYPE);
}

static bool paramSupportVarBinary(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_VARB_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_VAR_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_STRING_TYPE);
}

static bool paramSupportGeometry(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_GEOMETRY_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_ALL_TYPE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_VAR_TYPE);
}

static bool paramSupportValueNode(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_VALUE_NODE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_EXPR_NODE);
}

static bool paramSupportOperatorNode(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_EXPR_NODE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_OPERATOR_NODE);
}

static bool paramSupportFunctionNode(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_EXPR_NODE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_FUNCTION_NODE);
}

static bool paramSupportLogicConNode(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_EXPR_NODE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_LOGIC_CONDITION_NODE);
}

static bool paramSupportCaseWhenNode(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_EXPR_NODE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_CASE_WHEN_NODE);
}

static bool paramSupportColumnNode(uint64_t typeFlag) {
  return FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_EXPR_NODE) ||
         FUNC_MGT_TEST_MASK(typeFlag, FUNC_PARAM_SUPPORT_COLUMN_NODE);
}

static bool paramSupportNodeType(SNode* pNode, uint64_t typeFlag) {
  switch (pNode->type) {
    case QUERY_NODE_VALUE:
      return paramSupportValueNode(typeFlag);
    case QUERY_NODE_OPERATOR:
      return paramSupportOperatorNode(typeFlag);
    case QUERY_NODE_FUNCTION:
      return paramSupportFunctionNode(typeFlag);
    case QUERY_NODE_LOGIC_CONDITION:
      return paramSupportLogicConNode(typeFlag);
    case QUERY_NODE_CASE_WHEN:
      return paramSupportCaseWhenNode(typeFlag);
    case QUERY_NODE_COLUMN:
      return paramSupportColumnNode(typeFlag);
    default:
      return false;
  }
}

static bool paramSupportDataType(SDataType* pDataType, uint64_t typeFlag) {
  switch (pDataType->type) {
    case TSDB_DATA_TYPE_NULL:
      return paramSupportNull(typeFlag);
    case TSDB_DATA_TYPE_BOOL:
      return paramSupportBool(typeFlag);
    case TSDB_DATA_TYPE_TINYINT:
      return paramSupportTinyint(typeFlag);
    case TSDB_DATA_TYPE_SMALLINT:
      return paramSupportSmallint(typeFlag);
    case TSDB_DATA_TYPE_INT:
      return paramSupportInt(typeFlag);
    case TSDB_DATA_TYPE_BIGINT:
      return paramSupportBigint(typeFlag);
    case TSDB_DATA_TYPE_FLOAT:
      return paramSupportFloat(typeFlag);
    case TSDB_DATA_TYPE_DOUBLE:
      return paramSupportDouble(typeFlag);
    case TSDB_DATA_TYPE_VARCHAR:
      return paramSupportVarchar(typeFlag);
    case TSDB_DATA_TYPE_TIMESTAMP:
      return paramSupportTimestamp(typeFlag);
    case TSDB_DATA_TYPE_NCHAR:
      return paramSupportNchar(typeFlag);
    case TSDB_DATA_TYPE_UTINYINT:
      return paramSupportUTinyInt(typeFlag);
    case TSDB_DATA_TYPE_USMALLINT:
      return paramSupportUSmallInt(typeFlag);
    case TSDB_DATA_TYPE_UINT:
      return paramSupportUInt(typeFlag);
    case TSDB_DATA_TYPE_UBIGINT:
      return paramSupportUBigInt(typeFlag);
    case TSDB_DATA_TYPE_JSON:
      return paramSupportJSON(typeFlag);
    case TSDB_DATA_TYPE_VARBINARY:
      return paramSupportVarBinary(typeFlag);
    case TSDB_DATA_TYPE_GEOMETRY:
      return paramSupportGeometry(typeFlag);
    default:
      return false;
  }
}

int32_t validateParam(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  SNodeList*        paramList = pFunc->pParameterList;
  SFunctionParaInfo paramInfo = funcMgtBuiltins[pFunc->funcId].parameters;

  // check param num
  if ((paramInfo.maxParamNum != -1 && LIST_LENGTH(paramList) > paramInfo.maxParamNum) ||
      LIST_LENGTH(paramList) < paramInfo.minParamNum) {
    return invaildFuncParaNumErrMsg(pErrBuf, len, pFunc->functionName);
  }
  // check each param
  for (int32_t i = 0; i < paramInfo.paramInfoPattern; i++) {
    int32_t     paramIdx = 0;
    SParamInfo* paramPattern = paramInfo.inputParaInfo[i];
    bool        isMatch = true;
    while (!paramPattern[paramIdx].isLastParam) {
      for (int8_t j = paramPattern->startParam; j <= (paramPattern->endParam == -1 ? INT8_MAX : paramPattern->endParam); j++) {
        if (j >= LIST_LENGTH(paramList)) {
          isMatch = true;
          break;
        }
        SNode* pNode = nodesListGetNode(paramList, j - 1);
        // check node type
        if (!paramSupportNodeType(pNode, paramPattern[paramIdx].validNodeType)) {
          isMatch = false;
          break;
        }
        // check data type
        if (!paramSupportDataType(getSDataTypeFromNode(pNode), paramPattern[paramIdx].validDataType)) {
          isMatch = false;
          break;
        }
        // check range value
        if (paramPattern[paramIdx].hasRange) {
          if (pNode->type == QUERY_NODE_VALUE) {
            SValueNode* pVal = (SValueNode*)pNode;
            if ((double)pVal->datum.i < paramPattern[paramIdx].range.dMinVal ||
                (double)pVal->datum.i > paramPattern[paramIdx].range.dMaxVal) {
              isMatch = false;
              break;
            }
            pVal->notReserved = true;
          } else {
            // for other node type, range check should be done in process function
          }
        }
        // check fixed value
        if (paramPattern[paramIdx].isFixedValue) {
          if (pNode->type == QUERY_NODE_VALUE) {
            SValueNode* pVal = (SValueNode*)pNode;
            if (IS_NUMERIC_TYPE(getSDataTypeFromNode(pNode)->type)) {
              for (int32_t k = 0; k < paramPattern[paramIdx].fixedValueSize; k++) {
                if (pVal->datum.i == paramPattern[paramIdx].fixedNumValue[k]) {
                  isMatch = true;
                  break;
                } else {
                  isMatch = false;
                }
              }
            } else if (IS_STR_DATA_TYPE(getSDataTypeFromNode(pNode)->type)) {
              for (int32_t k = 0; k < paramPattern[paramIdx].fixedValueSize; k++) {
                if (strcasecmp(pVal->literal, paramPattern[paramIdx].fixedValue[k]) == 0) {
                  isMatch = true;
                  break;
                } else {
                  isMatch = false;
                }
              }
            }
            if (!isMatch) {
              break;
            }
            pVal->notReserved = true;
          } else {
            // for other node type, fixed value check should be done in process function
          }
        }
        // check isTs
        if (paramPattern[paramIdx].isTs) {
          if (nodeType(pNode) != QUERY_NODE_COLUMN || !IS_TIMESTAMP_TYPE(getSDataTypeFromNode(pNode)->type) ||
              !((SColumnNode*)pNode)->isPrimTs) {
            isMatch = false;
            break;
          }
        }
        // check isPK
        if (paramPattern[paramIdx].isPK) {
          if (nodeType(pNode) != QUERY_NODE_COLUMN || !IS_INTEGER_TYPE(getSDataTypeFromNode(pNode)->type) ||
              !((SColumnNode*)pNode)->isPk) {
            isMatch = false;
            break;
          }
        }
        // check hasColumn
        if (paramPattern[paramIdx].hasColumn) {
          if (!nodesExprHasColumn(pNode)) {
            isMatch = false;
            break;
          }
        }
      }

      paramIdx++;
      if (!isMatch) {
        break;
      }
    }
    if (isMatch) {
      return TSDB_CODE_SUCCESS;
    }
  }
  return invaildFuncParaTypeErrMsg(pErrBuf, len, pFunc->functionName);
}

// There is only one parameter of numeric type, and the return type is parameter type
static int32_t translateInOutNum(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  uint8_t paraType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->type;
  if (IS_NULL_TYPE(paraType)) {
    paraType = TSDB_DATA_TYPE_BIGINT;
  }
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[paraType].bytes, .type = paraType};
  return TSDB_CODE_SUCCESS;
}

// There is only one parameter of numeric type, and the return type is parameter type
static int32_t translateMinMax(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));

  SDataType* dataType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0));
  uint8_t paraType = IS_NULL_TYPE(dataType->type) ? TSDB_DATA_TYPE_BIGINT : dataType->type;
  int32_t bytes = IS_STR_DATA_TYPE(paraType) ? dataType->bytes : tDataTypes[paraType].bytes;
  pFunc->node.resType = (SDataType){.bytes = bytes, .type = paraType};
  return TSDB_CODE_SUCCESS;
}

// There is only one parameter of numeric type, and the return type is double type
static int32_t translateInNumOutDou(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};
  return TSDB_CODE_SUCCESS;
}

// There are two parameters of numeric type, and the return type is double type
static int32_t translateIn2NumOutDou(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};
  return TSDB_CODE_SUCCESS;
}

// There is only one parameter of string type, and the return type is parameter type
static int32_t translateInOutStr(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  SDataType* pRestType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0));

  pFunc->node.resType = (SDataType){.bytes = pRestType->bytes, .type = pRestType->type};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateTrimStr(SFunctionNode* pFunc, char* pErrBuf, int32_t len, bool isLtrim) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));

  SDataType* pRestType1 = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0));

  int32_t numOfSpaces = 0;
  SNode*  pParamNode1 = nodesListGetNode(pFunc->pParameterList, 0);
  // for select trim functions with constant value from table,
  // need to set the proper result result schema bytes to avoid
  // trailing garbage characters
  if (nodeType(pParamNode1) == QUERY_NODE_VALUE) {
    SValueNode* pValue = (SValueNode*)pParamNode1;
    numOfSpaces = countTrailingSpaces(pValue, isLtrim);
  }

  int32_t resBytes = pRestType1->bytes - numOfSpaces;
  pFunc->node.resType = (SDataType){.bytes = resBytes, .type = pRestType1->type};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateLtrim(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateTrimStr(pFunc, pErrBuf, len, true);
}

static int32_t translateRtrim(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateTrimStr(pFunc, pErrBuf, len, false);
}

static int32_t translateLogarithm(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateCount(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateSum(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));

  uint8_t paraType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->type;
  uint8_t resType = 0;
  if (IS_SIGNED_NUMERIC_TYPE(paraType) || TSDB_DATA_TYPE_BOOL == paraType || IS_NULL_TYPE(paraType)) {
    resType = TSDB_DATA_TYPE_BIGINT;
  } else if (IS_UNSIGNED_NUMERIC_TYPE(paraType)) {
    resType = TSDB_DATA_TYPE_UBIGINT;
  } else if (IS_FLOAT_TYPE(paraType)) {
    resType = TSDB_DATA_TYPE_DOUBLE;
  }

  pFunc->node.resType = (SDataType){.bytes = tDataTypes[resType].bytes, .type = resType};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateAvgPartial(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = getAvgInfoSize() + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateAvgMiddle(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = getAvgInfoSize() + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateAvgMerge(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};

  return TSDB_CODE_SUCCESS;
}

static int32_t translateAvgState(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = getAvgInfoSize() + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateAvgStateMerge(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = getAvgInfoSize() + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateStdPartial(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = getStdInfoSize() + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateStdMerge(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};

  return TSDB_CODE_SUCCESS;
}

static int32_t translateStdState(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = getStdInfoSize() + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateStdStateMerge(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = getStdInfoSize() + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};

  return TSDB_CODE_SUCCESS;
}

static int32_t translateWduration(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  // pseudo column do not need to check parameters
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT,
                                    .precision = pFunc->node.resType.precision};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateNowToday(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  // pseudo column do not need to check parameters

  // add database precision as param
  uint8_t dbPrec = pFunc->node.resType.precision;
  int32_t code = addUint8Param(&pFunc->pParameterList, dbPrec);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  pFunc->node.resType =
      (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_TIMESTAMP].bytes, .type = TSDB_DATA_TYPE_TIMESTAMP};
  return TSDB_CODE_SUCCESS;
}

static int32_t translatePi(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  pFunc->node.resType =
      (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateRand(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));

  if (!pFunc->dual) {
    int32_t code = addPseudoParam(&pFunc->pParameterList);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }
  }

  pFunc->node.resType =
      (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateRound(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));

  uint8_t paraType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->type;
  if (IS_NULL_TYPE(paraType)) {
    paraType = TSDB_DATA_TYPE_BIGINT;
  }
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[paraType].bytes, .type = paraType};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateTrunc(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  uint8_t paraType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->type;
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[paraType].bytes, .type = paraType};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateTimePseudoColumn(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  // pseudo column do not need to check parameters

  pFunc->node.resType =
      (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_TIMESTAMP].bytes, .type = TSDB_DATA_TYPE_TIMESTAMP,
                  .precision = pFunc->node.resType.precision};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateIsFilledPseudoColumn(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  // pseudo column do not need to check parameters

  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BOOL].bytes, .type = TSDB_DATA_TYPE_BOOL};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateTimezone(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  pFunc->node.resType = (SDataType){.bytes = TD_TIMEZONE_LEN, .type = TSDB_DATA_TYPE_BINARY};
  return TSDB_CODE_SUCCESS;
}

static int32_t translatePercentile(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  int32_t numOfParams = LIST_LENGTH(pFunc->pParameterList);

  // set result type
  if (numOfParams > 2) {
    pFunc->node.resType = (SDataType){.bytes = 3200, .type = TSDB_DATA_TYPE_VARCHAR};
  } else {
    pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};
  }
  return TSDB_CODE_SUCCESS;
}

static bool validateApercentileAlgo(const SValueNode* pVal) {
  if (TSDB_DATA_TYPE_BINARY != pVal->node.resType.type) {
    return false;
  }
  return (0 == strcasecmp(varDataVal(pVal->datum.p), "default") ||
          0 == strcasecmp(varDataVal(pVal->datum.p), "t-digest"));
}

static int32_t translateApercentile(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateApercentileImpl(SFunctionNode* pFunc, char* pErrBuf, int32_t len, bool isPartial) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  if (isPartial) {
    pFunc->node.resType =
        (SDataType){.bytes = getApercentileMaxSize() + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  } else {
    pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t translateApercentilePartial(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateApercentileImpl(pFunc, pErrBuf, len, true);
}

static int32_t translateApercentileMerge(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateApercentileImpl(pFunc, pErrBuf, len, false);
}

static int32_t translateTbnameColumn(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  // pseudo column do not need to check parameters
  pFunc->node.resType =
      (SDataType){.bytes = TSDB_TABLE_FNAME_LEN - 1 + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_VARCHAR};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateTbUidColumn(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  // pseudo column do not need to check parameters
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateVgIdColumn(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  // pseudo column do not need to check parameters
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_INT].bytes, .type = TSDB_DATA_TYPE_INT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateVgVerColumn(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateTopBot(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  // set result type
  SDataType* pType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0));
  pFunc->node.resType = (SDataType){.bytes = pType->bytes, .type = pType->type};
  return TSDB_CODE_SUCCESS;
}

static int32_t reserveFirstMergeParam(SNodeList* pRawParameters, SNode* pPartialRes, SNodeList** pParameters) {
  int32_t code = nodesListMakeAppend(pParameters, pPartialRes);
  if (TSDB_CODE_SUCCESS == code) {
    SNode* pNew = NULL;
    code = nodesCloneNode(nodesListGetNode(pRawParameters, 1), &pNew);
    if (TSDB_CODE_SUCCESS == code) {
      code = nodesListStrictAppend(*pParameters, pNew);
    }
  }
  return code;
}

int32_t topBotCreateMergeParam(SNodeList* pRawParameters, SNode* pPartialRes, SNodeList** pParameters) {
  return reserveFirstMergeParam(pRawParameters, pPartialRes, pParameters);
}

int32_t apercentileCreateMergeParam(SNodeList* pRawParameters, SNode* pPartialRes, SNodeList** pParameters) {
  int32_t code = reserveFirstMergeParam(pRawParameters, pPartialRes, pParameters);
  if (TSDB_CODE_SUCCESS == code && pRawParameters->length >= 3) {
    SNode* pNew = NULL;
    code = nodesCloneNode(nodesListGetNode(pRawParameters, 2), &pNew);
    if (TSDB_CODE_SUCCESS == code) {
      code = nodesListStrictAppend(*pParameters, pNew);
    }
  }
  return code;
}

static int32_t translateSpread(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateSpreadImpl(SFunctionNode* pFunc, char* pErrBuf, int32_t len, bool isPartial) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  if (isPartial) {
    pFunc->node.resType = (SDataType){.bytes = getSpreadInfoSize() + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  } else {
    pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t translateSpreadPartial(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateSpreadImpl(pFunc, pErrBuf, len, true);
}

static int32_t translateSpreadMerge(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateSpreadImpl(pFunc, pErrBuf, len, false);
}

static int32_t translateSpreadState(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = getSpreadInfoSize() + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateSpreadStateMerge(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = getSpreadInfoSize() + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateElapsed(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  int32_t numOfParams = LIST_LENGTH(pFunc->pParameterList);
  // param1
  if (2 == numOfParams) {
    uint8_t dbPrec = pFunc->node.resType.precision;


    // TODO(smj) : add this check to validateParam
    int32_t code = validateTimeUnitParam(dbPrec, (SValueNode*)nodesListGetNode(pFunc->pParameterList, 1));
    if (code == TSDB_CODE_FUNC_TIME_UNIT_TOO_SMALL) {
      return buildFuncErrMsg(pErrBuf, len, code,
                             "ELAPSED function time unit parameter should be greater than db precision");
    } else if (code == TSDB_CODE_FUNC_TIME_UNIT_INVALID) {
      return buildFuncErrMsg(
          pErrBuf, len, code,
          "ELAPSED function time unit parameter should be one of the following: [1b, 1u, 1a, 1s, 1m, 1h, 1d, 1w]");
    }
  }

  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateElapsedImpl(SFunctionNode* pFunc, char* pErrBuf, int32_t len, bool isPartial) {
  int32_t numOfParams = LIST_LENGTH(pFunc->pParameterList);

  if (isPartial) {
    if (1 != numOfParams && 2 != numOfParams) {
      return invaildFuncParaNumErrMsg(pErrBuf, len, pFunc->functionName);
    }

    uint8_t paraType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->type;
    if (!IS_TIMESTAMP_TYPE(paraType)) {
      return invaildFuncParaTypeErrMsg(pErrBuf, len, pFunc->functionName);
    }

    // param1
    if (2 == numOfParams) {
      SNode* pParamNode1 = nodesListGetNode(pFunc->pParameterList, 1);

      if (QUERY_NODE_VALUE != nodeType(pParamNode1)) {
        return invaildFuncParaTypeErrMsg(pErrBuf, len, pFunc->functionName);
      }

      SValueNode* pValue = (SValueNode*)pParamNode1;

      pValue->notReserved = true;

      paraType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 1))->type;
      if (!IS_INTEGER_TYPE(paraType)) {
        return invaildFuncParaTypeErrMsg(pErrBuf, len, pFunc->functionName);
      }

      if (pValue->datum.i == 0) {
        return buildFuncErrMsg(pErrBuf, len, TSDB_CODE_FUNC_FUNTION_ERROR,
                               "ELAPSED function time unit parameter should be greater than db precision");
      }
    }

    pFunc->node.resType =
        (SDataType){.bytes = getElapsedInfoSize() + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  } else {
    if (1 != numOfParams) {
      return invaildFuncParaNumErrMsg(pErrBuf, len, pFunc->functionName);
    }

    uint8_t paraType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->type;
    if (TSDB_DATA_TYPE_BINARY != paraType) {
      return invaildFuncParaTypeErrMsg(pErrBuf, len, pFunc->functionName);
    }
    pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t translateElapsedPartial(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
#if 0
  return translateElapsedImpl(pFunc, pErrBuf, len, true);
#endif
  return 0;
}

static int32_t translateElapsedMerge(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
#if 0
  return translateElapsedImpl(pFunc, pErrBuf, len, false);
#endif
  return 0;
}

static int32_t translateLeastSQR(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = LEASTSQUARES_BUFF_LENGTH, .type = TSDB_DATA_TYPE_BINARY};
  return TSDB_CODE_SUCCESS;
}

typedef enum { UNKNOWN_BIN = 0, USER_INPUT_BIN, LINEAR_BIN, LOG_BIN } EHistoBinType;

static int8_t validateHistogramBinType(char* binTypeStr) {
  int8_t binType;
  if (strcasecmp(binTypeStr, "user_input") == 0) {
    binType = USER_INPUT_BIN;
  } else if (strcasecmp(binTypeStr, "linear_bin") == 0) {
    binType = LINEAR_BIN;
  } else if (strcasecmp(binTypeStr, "log_bin") == 0) {
    binType = LOG_BIN;
  } else {
    binType = UNKNOWN_BIN;
  }

  return binType;
}

static int32_t validateHistogramBinDesc(char* binDescStr, int8_t binType, char* errMsg, int32_t msgLen) {
  const char* msg1 = "HISTOGRAM function requires four parameters";
  const char* msg3 = "HISTOGRAM function invalid format for binDesc parameter";
  const char* msg4 = "HISTOGRAM function binDesc parameter \"count\" should be in range [1, 1000]";
  const char* msg5 = "HISTOGRAM function bin/parameter should be in range [-DBL_MAX, DBL_MAX]";
  const char* msg6 = "HISTOGRAM function binDesc parameter \"width\" cannot be 0";
  const char* msg7 = "HISTOGRAM function binDesc parameter \"start\" cannot be 0 with \"log_bin\" type";
  const char* msg8 = "HISTOGRAM function binDesc parameter \"factor\" cannot be negative or equal to 0/1";
  const char* msg9 = "HISTOGRAM function out of memory";

  cJSON*  binDesc = cJSON_Parse(binDescStr);
  int32_t numOfBins;
  double* intervals;
  if (cJSON_IsObject(binDesc)) { /* linaer/log bins */
    int32_t numOfParams = cJSON_GetArraySize(binDesc);
    int32_t startIndex;
    if (numOfParams != 4) {
      (void)snprintf(errMsg, msgLen, "%s", msg1);
      cJSON_Delete(binDesc);
      return TSDB_CODE_FAILED;
    }

    cJSON* start = cJSON_GetObjectItem(binDesc, "start");
    cJSON* factor = cJSON_GetObjectItem(binDesc, "factor");
    cJSON* width = cJSON_GetObjectItem(binDesc, "width");
    cJSON* count = cJSON_GetObjectItem(binDesc, "count");
    cJSON* infinity = cJSON_GetObjectItem(binDesc, "infinity");

    if (!cJSON_IsNumber(start) || !cJSON_IsNumber(count) || !cJSON_IsBool(infinity)) {
      (void)snprintf(errMsg, msgLen, "%s", msg3);
      cJSON_Delete(binDesc);
      return TSDB_CODE_FAILED;
    }

    if (count->valueint <= 0 || count->valueint > 1000) {  // limit count to 1000
      (void)snprintf(errMsg, msgLen, "%s", msg4);
      cJSON_Delete(binDesc);
      return TSDB_CODE_FAILED;
    }

    if (isinf(start->valuedouble) || (width != NULL && isinf(width->valuedouble)) ||
        (factor != NULL && isinf(factor->valuedouble)) || (count != NULL && isinf(count->valuedouble))) {
      (void)snprintf(errMsg, msgLen, "%s", msg5);
      cJSON_Delete(binDesc);
      return TSDB_CODE_FAILED;
    }

    int32_t counter = (int32_t)count->valueint;
    if (infinity->valueint == false) {
      startIndex = 0;
      numOfBins = counter + 1;
    } else {
      startIndex = 1;
      numOfBins = counter + 3;
    }

    intervals = taosMemoryCalloc(numOfBins, sizeof(double));
    if (intervals == NULL) {
      (void)snprintf(errMsg, msgLen, "%s", msg9);
      cJSON_Delete(binDesc);
      return TSDB_CODE_FAILED;
    }
    if (cJSON_IsNumber(width) && factor == NULL && binType == LINEAR_BIN) {
      // linear bin process
      if (width->valuedouble == 0) {
        (void)snprintf(errMsg, msgLen, "%s", msg6);
        taosMemoryFree(intervals);
        cJSON_Delete(binDesc);
        return TSDB_CODE_FAILED;
      }
      for (int i = 0; i < counter + 1; ++i) {
        intervals[startIndex] = start->valuedouble + i * width->valuedouble;
        if (isinf(intervals[startIndex])) {
          (void)snprintf(errMsg, msgLen, "%s", msg5);
          taosMemoryFree(intervals);
          cJSON_Delete(binDesc);
          return TSDB_CODE_FAILED;
        }
        startIndex++;
      }
    } else if (cJSON_IsNumber(factor) && width == NULL && binType == LOG_BIN) {
      // log bin process
      if (start->valuedouble == 0) {
        (void)snprintf(errMsg, msgLen, "%s", msg7);
        taosMemoryFree(intervals);
        cJSON_Delete(binDesc);
        return TSDB_CODE_FAILED;
      }
      if (factor->valuedouble < 0 || factor->valuedouble == 0 || factor->valuedouble == 1) {
        (void)snprintf(errMsg, msgLen, "%s", msg8);
        taosMemoryFree(intervals);
        cJSON_Delete(binDesc);
        return TSDB_CODE_FAILED;
      }
      for (int i = 0; i < counter + 1; ++i) {
        intervals[startIndex] = start->valuedouble * pow(factor->valuedouble, i * 1.0);
        if (isinf(intervals[startIndex])) {
          (void)snprintf(errMsg, msgLen, "%s", msg5);
          taosMemoryFree(intervals);
          cJSON_Delete(binDesc);
          return TSDB_CODE_FAILED;
        }
        startIndex++;
      }
    } else {
      (void)snprintf(errMsg, msgLen, "%s", msg3);
      taosMemoryFree(intervals);
      cJSON_Delete(binDesc);
      return TSDB_CODE_FAILED;
    }

    if (infinity->valueint == true) {
      intervals[0] = -INFINITY;
      intervals[numOfBins - 1] = INFINITY;
      // in case of desc bin orders, -inf/inf should be swapped
      if (numOfBins < 4) {
        return TSDB_CODE_FAILED;
      }

      if (intervals[1] > intervals[numOfBins - 2]) {
        TSWAP(intervals[0], intervals[numOfBins - 1]);
      }
    }
  } else if (cJSON_IsArray(binDesc)) { /* user input bins */
    if (binType != USER_INPUT_BIN) {
      (void)snprintf(errMsg, msgLen, "%s", msg3);
      cJSON_Delete(binDesc);
      return TSDB_CODE_FAILED;
    }
    numOfBins = cJSON_GetArraySize(binDesc);
    intervals = taosMemoryCalloc(numOfBins, sizeof(double));
    if (intervals == NULL) {
      (void)snprintf(errMsg, msgLen, "%s", msg9);
      cJSON_Delete(binDesc);
      return terrno;
    }
    cJSON* bin = binDesc->child;
    if (bin == NULL) {
      (void)snprintf(errMsg, msgLen, "%s", msg3);
      taosMemoryFree(intervals);
      cJSON_Delete(binDesc);
      return TSDB_CODE_FAILED;
    }
    int i = 0;
    while (bin) {
      intervals[i] = bin->valuedouble;
      if (!cJSON_IsNumber(bin)) {
        (void)snprintf(errMsg, msgLen, "%s", msg3);
        taosMemoryFree(intervals);
        cJSON_Delete(binDesc);
        return TSDB_CODE_FAILED;
      }
      if (i != 0 && intervals[i] <= intervals[i - 1]) {
        (void)snprintf(errMsg, msgLen, "%s", msg3);
        taosMemoryFree(intervals);
        cJSON_Delete(binDesc);
        return TSDB_CODE_FAILED;
      }
      bin = bin->next;
      i++;
    }
  } else {
    (void)snprintf(errMsg, msgLen, "%s", msg3);
    cJSON_Delete(binDesc);
    return TSDB_CODE_FAILED;
  }

  cJSON_Delete(binDesc);
  taosMemoryFree(intervals);
  return TSDB_CODE_SUCCESS;
}

static int32_t translateHistogram(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  int32_t numOfParams = LIST_LENGTH(pFunc->pParameterList);

  int8_t binType;
  char*  binDesc;

  // TODO(smj) : add this to validateParam
  for (int32_t i = 1; i < numOfParams; ++i) {
    SValueNode* pValue = (SValueNode*)nodesListGetNode(pFunc->pParameterList, i);

    pValue->notReserved = true;

    if (i == 1) {
      binType = validateHistogramBinType(varDataVal(pValue->datum.p));
      if (binType == UNKNOWN_BIN) {
        return buildFuncErrMsg(pErrBuf, len, TSDB_CODE_FUNC_FUNTION_ERROR,
                               "HISTOGRAM function binType parameter should be "
                               "\"user_input\", \"log_bin\" or \"linear_bin\"");
      }
    }

    if (i == 2) {
      char errMsg[128] = {0};
      binDesc = varDataVal(pValue->datum.p);
      if (TSDB_CODE_SUCCESS != validateHistogramBinDesc(binDesc, binType, errMsg, (int32_t)sizeof(errMsg))) {
        return buildFuncErrMsg(pErrBuf, len, TSDB_CODE_FUNC_FUNTION_ERROR, errMsg);
      }
    }
  }

  pFunc->node.resType = (SDataType){.bytes = 512, .type = TSDB_DATA_TYPE_BINARY};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateHistogramImpl(SFunctionNode* pFunc, char* pErrBuf, int32_t len, bool isPartial) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  int32_t numOfParams = LIST_LENGTH(pFunc->pParameterList);
  if (isPartial) {
    int8_t binType;
    char*  binDesc;
    for (int32_t i = 1; i < numOfParams; ++i) {
      SValueNode* pValue = (SValueNode *)nodesListGetNode(pFunc->pParameterList, i);

      pValue->notReserved = true;

      if (i == 1) {
        binType = validateHistogramBinType(varDataVal(pValue->datum.p));
        if (binType == UNKNOWN_BIN) {
          return buildFuncErrMsg(pErrBuf, len, TSDB_CODE_FUNC_FUNTION_ERROR,
                                 "HISTOGRAM function binType parameter should be "
                                 "\"user_input\", \"log_bin\" or \"linear_bin\"");
        }
      }

      if (i == 2) {
        char errMsg[128] = {0};
        binDesc = varDataVal(pValue->datum.p);
        if (TSDB_CODE_SUCCESS != validateHistogramBinDesc(binDesc, binType, errMsg, (int32_t)sizeof(errMsg))) {
          return buildFuncErrMsg(pErrBuf, len, TSDB_CODE_FUNC_FUNTION_ERROR, errMsg);
        }
      }
    }

    pFunc->node.resType =
        (SDataType){.bytes = getHistogramInfoSize() + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  } else {
    pFunc->node.resType = (SDataType){.bytes = 512, .type = TSDB_DATA_TYPE_BINARY};
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t translateHistogramPartial(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateHistogramImpl(pFunc, pErrBuf, len, true);
}

static int32_t translateHistogramMerge(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateHistogramImpl(pFunc, pErrBuf, len, false);
}

static int32_t translateHLL(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateHLLImpl(SFunctionNode* pFunc, char* pErrBuf, int32_t len, bool isPartial) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  if (isPartial) {
    pFunc->node.resType =
        (SDataType){.bytes = getHistogramInfoSize() + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  } else {
    pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT};
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t translateHLLPartial(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateHLLImpl(pFunc, pErrBuf, len, true);
}

static int32_t translateHLLMerge(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateHLLImpl(pFunc, pErrBuf, len, false);
}

static int32_t translateHLLState(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateHLLPartial(pFunc, pErrBuf, len);
}

static int32_t translateHLLStateMerge(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType =
    (SDataType){.bytes = getHistogramInfoSize() + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  return TSDB_CODE_SUCCESS;
}

static bool validateStateOper(const SValueNode* pVal) {
  if (TSDB_DATA_TYPE_BINARY != pVal->node.resType.type) {
    return false;
  }
  if (strlen(varDataVal(pVal->datum.p)) == 2) {
    return (
        0 == strncasecmp(varDataVal(pVal->datum.p), "GT", 2) || 0 == strncasecmp(varDataVal(pVal->datum.p), "GE", 2) ||
        0 == strncasecmp(varDataVal(pVal->datum.p), "LT", 2) || 0 == strncasecmp(varDataVal(pVal->datum.p), "LE", 2) ||
        0 == strncasecmp(varDataVal(pVal->datum.p), "EQ", 2) || 0 == strncasecmp(varDataVal(pVal->datum.p), "NE", 2));
  }
  return false;
}

static int32_t translateStateCount(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  // set result type
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateStateDuration(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  int32_t numOfParams = LIST_LENGTH(pFunc->pParameterList);

  if (numOfParams == 4) {
    uint8_t dbPrec = pFunc->node.resType.precision;

    int32_t code = validateTimeUnitParam(dbPrec, (SValueNode*)nodesListGetNode(pFunc->pParameterList, 3));
    if (code == TSDB_CODE_FUNC_TIME_UNIT_TOO_SMALL) {
      return buildFuncErrMsg(pErrBuf, len, code,
                             "STATEDURATION function time unit parameter should be greater than db precision");
    } else if (code == TSDB_CODE_FUNC_TIME_UNIT_INVALID) {
      return buildFuncErrMsg(pErrBuf, len, code,
                             "STATEDURATION function time unit parameter should be one of the following: [1b, 1u, 1a, "
                             "1s, 1m, 1h, 1d, 1w]");
    }
  }

  // set result type
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateCsum(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  uint8_t colType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->type;
  uint8_t resType;
  if (IS_SIGNED_NUMERIC_TYPE(colType)) {
    resType = TSDB_DATA_TYPE_BIGINT;
  } else if (IS_UNSIGNED_NUMERIC_TYPE(colType)) {
    resType = TSDB_DATA_TYPE_UBIGINT;
  } else if (IS_FLOAT_TYPE(colType)) {
    resType = TSDB_DATA_TYPE_DOUBLE;
  } else {
    return invaildFuncParaTypeErrMsg(pErrBuf, len, pFunc->functionName);
  }
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[resType].bytes, .type = resType};
  return TSDB_CODE_SUCCESS;
}

static EFuncReturnRows csumEstReturnRows(SFunctionNode* pFunc) { return FUNC_RETURN_ROWS_N; }

static int32_t translateMavg(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateSample(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));

  SDataType* pSDataType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0));
  uint8_t    colType = pSDataType->type;

  // set result type
  pFunc->node.resType = (SDataType){.bytes = IS_STR_DATA_TYPE(colType) ? pSDataType->bytes : tDataTypes[colType].bytes,
                                    .type = colType};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateTail(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));

  SDataType* pSDataType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0));
  uint8_t    colType = pSDataType->type;

  // set result type
  pFunc->node.resType = (SDataType){.bytes = IS_STR_DATA_TYPE(colType) ? pSDataType->bytes : tDataTypes[colType].bytes,
                                    .type = colType};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateDerivative(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};
  return TSDB_CODE_SUCCESS;
}

static EFuncReturnRows derivativeEstReturnRows(SFunctionNode* pFunc) {
  return 1 == ((SValueNode*)nodesListGetNode(pFunc->pParameterList, 2))->datum.i ? FUNC_RETURN_ROWS_INDEFINITE
                                                                                 : FUNC_RETURN_ROWS_N_MINUS_1;
}

static int32_t translateIrate(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  // add database precision as param
  uint8_t dbPrec = pFunc->node.resType.precision;
  int32_t code = addUint8Param(&pFunc->pParameterList, dbPrec);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateIrateImpl(SFunctionNode* pFunc, char* pErrBuf, int32_t len, bool isPartial) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  if (isPartial) {
    int32_t pkBytes = (pFunc->hasPk) ? pFunc->pkBytes : 0;
    pFunc->node.resType = (SDataType){.bytes = getIrateInfoSize(pkBytes) + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  } else {
    pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_DOUBLE].bytes, .type = TSDB_DATA_TYPE_DOUBLE};

    // add database precision as param
    uint8_t dbPrec = pFunc->node.resType.precision;
    int32_t code = addUint8Param(&pFunc->pParameterList, dbPrec);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t translateIratePartial(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateIrateImpl(pFunc, pErrBuf, len, true);
}

static int32_t translateIrateMerge(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateIrateImpl(pFunc, pErrBuf, len, false);
}

static int32_t translateInterp(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = ((SExprNode*)nodesListGetNode(pFunc->pParameterList, 0))->resType;
  return TSDB_CODE_SUCCESS;
}

static EFuncReturnRows interpEstReturnRows(SFunctionNode* pFunc) {
  int32_t numOfParams = LIST_LENGTH(pFunc->pParameterList);
  if (1 < numOfParams && 1 == ((SValueNode*)nodesListGetNode(pFunc->pParameterList, 1))->datum.i) {
    return FUNC_RETURN_ROWS_INDEFINITE;
  } else {
    return FUNC_RETURN_ROWS_N;
  }
}

static int32_t translateFirstLast(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  // forbid null as first/last input, since first(c0, null, 1) may have different number of input
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = *getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0));
  return TSDB_CODE_SUCCESS;
}

static int32_t translateFirstLastImpl(SFunctionNode* pFunc, char* pErrBuf, int32_t len, bool isPartial) {
  // first(col_list) will be rewritten as first(col)
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  SNode*  pPara = nodesListGetNode(pFunc->pParameterList, 0);
  int32_t paraBytes = getSDataTypeFromNode(pPara)->bytes;
  if (isPartial) {
    int32_t pkBytes = (pFunc->hasPk) ? pFunc->pkBytes : 0;
    pFunc->node.resType =
        (SDataType){.bytes = getFirstLastInfoSize(paraBytes, pkBytes) + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  } else {
    pFunc->node.resType = ((SExprNode*)pPara)->resType;
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t translateFirstLastPartial(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateFirstLastImpl(pFunc, pErrBuf, len, true);
}

static int32_t translateFirstLastMerge(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateFirstLastImpl(pFunc, pErrBuf, len, false);
}

static int32_t translateFirstLastState(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  SNode*  pPara = nodesListGetNode(pFunc->pParameterList, 0);
  int32_t paraBytes = getSDataTypeFromNode(pPara)->bytes;

  int32_t pkBytes = (pFunc->hasPk) ? pFunc->pkBytes : 0;
  pFunc->node.resType =
    (SDataType){.bytes = getFirstLastInfoSize(paraBytes, pkBytes) + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_BINARY};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateFirstLastStateMerge(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  SNode*  pPara = nodesListGetNode(pFunc->pParameterList, 0);
  int32_t paraBytes = getSDataTypeFromNode(pPara)->bytes;

  pFunc->node.resType = (SDataType){.bytes = paraBytes, .type = TSDB_DATA_TYPE_BINARY};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateUniqueMode(SFunctionNode* pFunc, char* pErrBuf, int32_t len, bool isUnique) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = *getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0));
  return TSDB_CODE_SUCCESS;
}

static int32_t translateUnique(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateUniqueMode(pFunc, pErrBuf, len, true);
}

static int32_t translateMode(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateUniqueMode(pFunc, pErrBuf, len, false);
}

static int32_t translateForecast(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  int32_t numOfParams = LIST_LENGTH(pFunc->pParameterList);
  if (2 != numOfParams && 1 != numOfParams) {
    return invaildFuncParaNumErrMsg(pErrBuf, len, "FORECAST require 1 or 2 parameters");
  }

  uint8_t valType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->type;
  if (!IS_MATHABLE_TYPE(valType)) {
    return invaildFuncParaTypeErrMsg(pErrBuf, len, "FORECAST only support mathable column");
  }

  if (numOfParams == 2) {
    uint8_t optionType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 1))->type;
    if (TSDB_DATA_TYPE_BINARY != optionType) {
      return invaildFuncParaTypeErrMsg(pErrBuf, len, "FORECAST option should be varchar");
    }

    SNode* pOption = nodesListGetNode(pFunc->pParameterList, 1);
    if (QUERY_NODE_VALUE != nodeType(pOption)) {
      return invaildFuncParaTypeErrMsg(pErrBuf, len, "FORECAST option should be value");
    }

    SValueNode* pValue = (SValueNode*)pOption;
    if (!taosAnalGetOptStr(pValue->literal, "algo", NULL, 0) != 0) {
      return invaildFuncParaValueErrMsg(pErrBuf, len, "FORECAST option should include algo field");
    }

    pValue->notReserved = true;
  }

  pFunc->node.resType = (SDataType){.bytes = tDataTypes[valType].bytes, .type = valType};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateForecastConf(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_FLOAT].bytes, .type = TSDB_DATA_TYPE_FLOAT};
  return TSDB_CODE_SUCCESS;
}

static EFuncReturnRows forecastEstReturnRows(SFunctionNode* pFunc) { return FUNC_RETURN_ROWS_N; }

static int32_t translateDiff(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  uint8_t colType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->type;

  uint8_t resType;
  if (IS_SIGNED_NUMERIC_TYPE(colType) || IS_TIMESTAMP_TYPE(colType) || TSDB_DATA_TYPE_BOOL == colType) {
    resType = TSDB_DATA_TYPE_BIGINT;
  } else if (IS_UNSIGNED_NUMERIC_TYPE(colType)) {
    resType = TSDB_DATA_TYPE_BIGINT;
  } else {
    resType = TSDB_DATA_TYPE_DOUBLE;
  }
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[resType].bytes, .type = resType};
  return TSDB_CODE_SUCCESS;
}

static EFuncReturnRows diffEstReturnRows(SFunctionNode* pFunc) {
  if (1 == LIST_LENGTH(pFunc->pParameterList)) {
    return FUNC_RETURN_ROWS_N_MINUS_1;
  }
  return 1 < ((SValueNode*)nodesListGetNode(pFunc->pParameterList, 1))->datum.i ? FUNC_RETURN_ROWS_INDEFINITE
                                                                                 : FUNC_RETURN_ROWS_N_MINUS_1;
}

static int32_t translateLength(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateCharLength(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateConcatImpl(SFunctionNode* pFunc, char* pErrBuf, int32_t len, int32_t minParaNum,
                                   int32_t maxParaNum, bool hasSep) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  int32_t numOfParams = LIST_LENGTH(pFunc->pParameterList);

  uint8_t resultType = TSDB_DATA_TYPE_BINARY;
  int32_t resultBytes = 0;
  int32_t sepBytes = 0;

  /* For concat/concat_ws function, if params have NCHAR type, promote the final result to NCHAR */
  for (int32_t i = 0; i < numOfParams; ++i) {
    SNode*  pPara = nodesListGetNode(pFunc->pParameterList, i);
    uint8_t paraType = getSDataTypeFromNode(pPara)->type;
    if (TSDB_DATA_TYPE_NCHAR == paraType) {
      resultType = paraType;
    }
  }

  for (int32_t i = 0; i < numOfParams; ++i) {
    SNode*  pPara = nodesListGetNode(pFunc->pParameterList, i);
    uint8_t paraType = getSDataTypeFromNode(pPara)->type;
    int32_t paraBytes = getSDataTypeFromNode(pPara)->bytes;
    int32_t factor = 1;
    if (IS_NULL_TYPE(paraType)) {
      resultType = TSDB_DATA_TYPE_VARCHAR;
      resultBytes = 0;
      sepBytes = 0;
      break;
    }
    if (TSDB_DATA_TYPE_NCHAR == resultType && TSDB_DATA_TYPE_VARCHAR == paraType) {
      factor *= TSDB_NCHAR_SIZE;
    }
    resultBytes += paraBytes * factor;

    if (i == 0) {
      sepBytes = paraBytes * factor;
    }
  }

  if (hasSep) {
    resultBytes += sepBytes * (numOfParams - 3);
  }

  pFunc->node.resType = (SDataType){.bytes = resultBytes, .type = resultType};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateConcat(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateConcatImpl(pFunc, pErrBuf, len, 2, 8, false);
}

static int32_t translateConcatWs(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  return translateConcatImpl(pFunc, pErrBuf, len, 3, 9, true);
}

static int32_t translateSubstr(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  SExprNode* pPara0 = (SExprNode*)nodesListGetNode(pFunc->pParameterList, 0);
  pFunc->node.resType = (SDataType){.bytes = pPara0->resType.bytes, .type = pPara0->resType.type};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateSubstrIdx(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  SExprNode* pPara0 = (SExprNode*)nodesListGetNode(pFunc->pParameterList, 0);
  pFunc->node.resType = (SDataType){.bytes = pPara0->resType.bytes, .type = pPara0->resType.type};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateChar(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  int32_t numOfParams = LIST_LENGTH(pFunc->pParameterList);
  pFunc->node.resType = (SDataType){.bytes = 4 * numOfParams + 2, .type = TSDB_DATA_TYPE_VARCHAR};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateAscii(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_UTINYINT].bytes, .type = TSDB_DATA_TYPE_UTINYINT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translatePosition(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateTrim(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));


  uint8_t para0Type = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->type;
  int32_t resLen = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->bytes;
  uint8_t type = para0Type;

  if (2 == LIST_LENGTH(pFunc->pParameterList)) {
    uint8_t para1Type = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 1))->type;
    resLen = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 1))->bytes;
    type = para1Type;
  }
  if (type == TSDB_DATA_TYPE_NCHAR) {
    resLen *= TSDB_NCHAR_SIZE;
  }
  uint8_t trimType = pFunc->trimType;
  int32_t code = addUint8Param(&pFunc->pParameterList, trimType);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }
  pFunc->node.resType = (SDataType){.bytes = resLen, .type = type};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateReplace(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));

  uint8_t orgType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->type;
  uint8_t fromType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 1))->type;
  uint8_t toType = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 2))->type;
  int32_t orgLen = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->bytes;
  int32_t fromLen = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 1))->bytes;
  int32_t toLen = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 2))->bytes;

  int32_t resLen;
  // Since we don't know the accurate length of result, estimate the maximum length here.
  // To make the resLen bigger, we should make fromLen smaller and toLen bigger.
  if (orgType == TSDB_DATA_TYPE_VARBINARY && fromType != orgType) {
    fromLen = fromLen / TSDB_NCHAR_SIZE;
  }
  if (orgType == TSDB_DATA_TYPE_NCHAR && toType != orgType) {
    toLen = toLen * TSDB_NCHAR_SIZE;
  }
  resLen = TMAX(orgLen, orgLen + orgLen / fromLen * (toLen - fromLen));
  pFunc->node.resType = (SDataType){.bytes = resLen, .type = orgType};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateRepeat(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));

  uint8_t type = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->type;
  int32_t orgLen = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->bytes;
  int32_t count = TMAX((int32_t)((SValueNode*)nodesListGetNode(pFunc->pParameterList, 1))->datum.i, 1);

  int32_t resLen = orgLen * count;
  pFunc->node.resType = (SDataType){.bytes = resLen, .type = type};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateCast(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  // The number of parameters has been limited by the syntax definition

  SExprNode* pPara0 = (SExprNode*)nodesListGetNode(pFunc->pParameterList, 0);
  uint8_t para0Type = pPara0->resType.type;
  if (TSDB_DATA_TYPE_VARBINARY == para0Type) {
    return invaildFuncParaTypeErrMsg(pErrBuf, len, pFunc->functionName);
  }

  // The function return type has been set during syntax parsing
  uint8_t para2Type = pFunc->node.resType.type;

  int32_t para2Bytes = pFunc->node.resType.bytes;
  if (IS_STR_DATA_TYPE(para2Type)) {
    para2Bytes -= VARSTR_HEADER_SIZE;
  }
  if (para2Bytes <= 0 ||
      para2Bytes > TSDB_MAX_BINARY_LEN - VARSTR_HEADER_SIZE) {  // cast dst var type length limits to 4096 bytes
    if (TSDB_DATA_TYPE_NCHAR == para2Type) {
      return buildFuncErrMsg(pErrBuf, len, TSDB_CODE_FUNC_FUNTION_ERROR,
                             "CAST function converted length should be in range (0, %d] NCHARS",
                             (TSDB_MAX_BINARY_LEN - VARSTR_HEADER_SIZE)/TSDB_NCHAR_SIZE);
    } else {
      return buildFuncErrMsg(pErrBuf, len, TSDB_CODE_FUNC_FUNTION_ERROR,
                             "CAST function converted length should be in range (0, %d] bytes",
                             TSDB_MAX_BINARY_LEN - VARSTR_HEADER_SIZE);
    }
  }

  // add database precision as param
  uint8_t dbPrec = pFunc->node.resType.precision;
  int32_t code = addUint8Param(&pFunc->pParameterList, dbPrec);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t translateToIso8601(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  int32_t numOfParams = LIST_LENGTH(pFunc->pParameterList);
  // param1
  if (numOfParams == 2) {
    SValueNode* pValue = (SValueNode*)nodesListGetNode(pFunc->pParameterList, 1);
    if (!validateTimezoneFormat(pValue)) {
      return buildFuncErrMsg(pErrBuf, len, TSDB_CODE_FUNC_FUNTION_ERROR, "Invalid timzone format");
    }
  } else {  // add default client timezone
    int32_t code = addTimezoneParam(pFunc->pParameterList);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }
  }

  // set result type
  pFunc->node.resType = (SDataType){.bytes = 64, .type = TSDB_DATA_TYPE_BINARY};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateToUnixtimestamp(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  int32_t numOfParams = LIST_LENGTH(pFunc->pParameterList);
  int16_t resType = TSDB_DATA_TYPE_BIGINT;
  if (2 == numOfParams) {
    SValueNode* pValue = (SValueNode*)nodesListGetNode(pFunc->pParameterList, 1);
    if (pValue->datum.i == 1) {
      resType = TSDB_DATA_TYPE_TIMESTAMP;
    } else if (pValue->datum.i == 0) {
      resType = TSDB_DATA_TYPE_BIGINT;
    } else {
      return buildFuncErrMsg(pErrBuf, len, TSDB_CODE_FUNC_FUNTION_ERROR,
                             "TO_UNIXTIMESTAMP function second parameter should be 0/1");
    }
  }

  // add database precision as param
  uint8_t dbPrec = pFunc->node.resType.precision;
  int32_t code = addUint8Param(&pFunc->pParameterList, dbPrec);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  pFunc->node.resType = (SDataType){.bytes = tDataTypes[resType].bytes, .type = resType};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateToTimestamp(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType =
      (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_TIMESTAMP].bytes, .type = TSDB_DATA_TYPE_TIMESTAMP};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateToChar(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = 4096, .type = TSDB_DATA_TYPE_VARCHAR};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateTimeTruncate(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  uint8_t dbPrec = pFunc->node.resType.precision;
  int32_t code = validateTimeUnitParam(dbPrec, (SValueNode*)nodesListGetNode(pFunc->pParameterList, 1));
  if (code == TSDB_CODE_FUNC_TIME_UNIT_TOO_SMALL) {
    return buildFuncErrMsg(pErrBuf, len, code,
                           "TIMETRUNCATE function time unit parameter should be greater than db precision");
  } else if (code == TSDB_CODE_FUNC_TIME_UNIT_INVALID) {
    return buildFuncErrMsg(
        pErrBuf, len, code,
        "TIMETRUNCATE function time unit parameter should be one of the following: [1b, 1u, 1a, 1s, 1m, 1h, 1d, 1w]");
  }

  // add database precision as param

  code = addUint8Param(&pFunc->pParameterList, dbPrec);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  // add client timezone as param
  code = addTimezoneParam(pFunc->pParameterList);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  pFunc->node.resType =
      (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_TIMESTAMP].bytes, .type = TSDB_DATA_TYPE_TIMESTAMP};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateTimeDiff(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  int32_t numOfParams = LIST_LENGTH(pFunc->pParameterList);

  uint8_t para2Type;
  if (3 == numOfParams) {
    para2Type = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 2))->type;
  }

  // add database precision as param
  uint8_t dbPrec = pFunc->node.resType.precision;

  if (3 == numOfParams && !IS_NULL_TYPE(para2Type)) {
    int32_t code = validateTimeUnitParam(dbPrec, (SValueNode*)nodesListGetNode(pFunc->pParameterList, 2));
    if (code == TSDB_CODE_FUNC_TIME_UNIT_TOO_SMALL) {
      return buildFuncErrMsg(pErrBuf, len, code,
                             "TIMEDIFF function time unit parameter should be greater than db precision");
    } else if (code == TSDB_CODE_FUNC_TIME_UNIT_INVALID) {
      return buildFuncErrMsg(
          pErrBuf, len, code,
          "TIMEDIFF function time unit parameter should be one of the following: [1b, 1u, 1a, 1s, 1m, 1h, 1d, 1w]");
    }
  }

  int32_t code = addUint8Param(&pFunc->pParameterList, dbPrec);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateWeekday(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));

  // add database precision as param
  uint8_t dbPrec = pFunc->node.resType.precision;

  int32_t code = addUint8Param(&pFunc->pParameterList, dbPrec);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  pFunc->node.resType =
      (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateWeek(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));

  // add database precision as param
  uint8_t dbPrec = pFunc->node.resType.precision;

  int32_t code = addUint8Param(&pFunc->pParameterList, dbPrec);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  pFunc->node.resType =
      (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateWeekofyear(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));

  // add database precision as param
  uint8_t dbPrec = pFunc->node.resType.precision;

  int32_t code = addUint8Param(&pFunc->pParameterList, dbPrec);
  if (code != TSDB_CODE_SUCCESS) {
    return code;
  }

  pFunc->node.resType =
      (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateToJson(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_JSON].bytes, .type = TSDB_DATA_TYPE_JSON};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateInStrOutGeom(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_GEOMETRY].bytes, .type = TSDB_DATA_TYPE_GEOMETRY};

  return TSDB_CODE_SUCCESS;
}

static int32_t translateInGeomOutStr(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  if (1 != LIST_LENGTH(pFunc->pParameterList)) {
    return invaildFuncParaNumErrMsg(pErrBuf, len, pFunc->functionName);
  }

  uint8_t para1Type = getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0))->type;
  if (para1Type != TSDB_DATA_TYPE_GEOMETRY && !IS_NULL_TYPE(para1Type)) {
    return invaildFuncParaTypeErrMsg(pErrBuf, len, pFunc->functionName);
  }

  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_VARCHAR].bytes, .type = TSDB_DATA_TYPE_VARCHAR};

  return TSDB_CODE_SUCCESS;
}

static int32_t translateIn2NumOutGeom(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_GEOMETRY].bytes, .type = TSDB_DATA_TYPE_GEOMETRY};

  return TSDB_CODE_SUCCESS;
}

static int32_t translateIn2GeomOutBool(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BOOL].bytes, .type = TSDB_DATA_TYPE_BOOL};

  return TSDB_CODE_SUCCESS;
}

static int32_t translateSelectValue(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  pFunc->node.resType = ((SExprNode*)nodesListGetNode(pFunc->pParameterList, 0))->resType;
  return TSDB_CODE_SUCCESS;
}

static int32_t translateBlockDistFunc(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  pFunc->node.resType = (SDataType){.bytes = 128, .type = TSDB_DATA_TYPE_VARCHAR};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateBlockDistInfoFunc(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  pFunc->node.resType = (SDataType){.bytes = 128, .type = TSDB_DATA_TYPE_VARCHAR};
  return TSDB_CODE_SUCCESS;
}

static bool getBlockDistFuncEnv(SFunctionNode* UNUSED_PARAM(pFunc), SFuncExecEnv* pEnv) {
  pEnv->calcMemSize = sizeof(STableBlockDistInfo);
  return true;
}

static int32_t translateGroupKey(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = *getSDataTypeFromNode(nodesListGetNode(pFunc->pParameterList, 0));
  return TSDB_CODE_SUCCESS;
}

static int32_t translateDatabaseFunc(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  pFunc->node.resType = (SDataType){.bytes = TSDB_DB_NAME_LEN, .type = TSDB_DATA_TYPE_VARCHAR};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateClientVersionFunc(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  pFunc->node.resType = (SDataType){.bytes = TSDB_VERSION_LEN, .type = TSDB_DATA_TYPE_VARCHAR};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateServerVersionFunc(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  pFunc->node.resType = (SDataType){.bytes = TSDB_VERSION_LEN, .type = TSDB_DATA_TYPE_VARCHAR};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateServerStatusFunc(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_INT].bytes, .type = TSDB_DATA_TYPE_INT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateCurrentUserFunc(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  pFunc->node.resType = (SDataType){.bytes = TSDB_USER_LEN, .type = TSDB_DATA_TYPE_VARCHAR};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateUserFunc(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  pFunc->node.resType = (SDataType){.bytes = TSDB_USER_LEN, .type = TSDB_DATA_TYPE_VARCHAR};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateTagsPseudoColumn(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  // The _tags pseudo-column will be expanded to the actual tags on the client side
  return TSDB_CODE_SUCCESS;
}

static int32_t translateTableCountPseudoColumn(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  pFunc->node.resType = (SDataType){.bytes = tDataTypes[TSDB_DATA_TYPE_BIGINT].bytes, .type = TSDB_DATA_TYPE_BIGINT};
  return TSDB_CODE_SUCCESS;
}

static int32_t translateMd5(SFunctionNode* pFunc, char* pErrBuf, int32_t len) {
  FUNC_ERR_RET(validateParam(pFunc, pErrBuf, len));
  pFunc->node.resType = (SDataType){.bytes = MD5_OUTPUT_LEN + VARSTR_HEADER_SIZE, .type = TSDB_DATA_TYPE_VARCHAR};
  return TSDB_CODE_SUCCESS;
}

// clang-format off
const SBuiltinFuncDefinition funcMgtBuiltins[] = {
  {
    .name = "count",
    .type = FUNCTION_TYPE_COUNT,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SPECIAL_DATA_REQUIRED | FUNC_MGT_IGNORE_NULL_FUNC | FUNC_MGT_TSMA_FUNC | FUNC_MGT_COUNT_LIKE_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateCount,
    .dataRequiredFunc = countDataRequired,
    .getEnvFunc   = getCountFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = countFunction,
    .sprocessFunc = countScalarFunction,
    .finalizeFunc = functionFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = countInvertFunction,
#endif
    .combineFunc  = combineFunction,
    .pPartialFunc = "count",
    .pStateFunc = "count",
    .pMergeFunc   = "sum"
  },
  {
    .name = "sum",
    .type = FUNCTION_TYPE_SUM,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SPECIAL_DATA_REQUIRED | FUNC_MGT_IGNORE_NULL_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE | FUNC_PARAM_SUPPORT_DOUBLE_TYPE | FUNC_PARAM_SUPPORT_UBIGINT_TYPE}},
    .translateFunc = translateSum,
    .dataRequiredFunc = statisDataRequired,
    .getEnvFunc   = getSumFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = sumFunction,
    .sprocessFunc = sumScalarFunction,
    .finalizeFunc = functionFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = sumInvertFunction,
#endif
    .combineFunc  = sumCombine,
    .pPartialFunc = "sum",
    .pStateFunc = "sum",
    .pMergeFunc   = "sum"
  },
  {
    .name = "min",
    .type = FUNCTION_TYPE_MIN,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SPECIAL_DATA_REQUIRED | FUNC_MGT_SELECT_FUNC | FUNC_MGT_IGNORE_NULL_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_STRING_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_STRING_TYPE}},
    .translateFunc = translateMinMax,
    .dataRequiredFunc = statisDataRequired,
    .getEnvFunc   = getMinmaxFuncEnv,
    .initFunc     = minmaxFunctionSetup,
    .processFunc  = minFunction,
    .sprocessFunc = minScalarFunction,
    .finalizeFunc = minmaxFunctionFinalize,
    .combineFunc  = minCombine,
    .pPartialFunc = "min",
    .pStateFunc = "min",
    .pMergeFunc   = "min"
  },
  {
    .name = "max",
    .type = FUNCTION_TYPE_MAX,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SPECIAL_DATA_REQUIRED | FUNC_MGT_SELECT_FUNC | FUNC_MGT_IGNORE_NULL_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_STRING_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_STRING_TYPE}},
    .translateFunc = translateMinMax,
    .dataRequiredFunc = statisDataRequired,
    .getEnvFunc   = getMinmaxFuncEnv,
    .initFunc     = minmaxFunctionSetup,
    .processFunc  = maxFunction,
    .sprocessFunc = maxScalarFunction,
    .finalizeFunc = minmaxFunctionFinalize,
    .combineFunc  = maxCombine,
    .pPartialFunc = "max",
    .pStateFunc = "max",
    .pMergeFunc   = "max"
  },
  {
    .name = "stddev",
    .type = FUNCTION_TYPE_STDDEV,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .getEnvFunc   = getStdFuncEnv,
    .initFunc     = stdFunctionSetup,
    .processFunc  = stdFunction,
    .sprocessFunc = stdScalarFunction,
    .finalizeFunc = stddevFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = stdInvertFunction,
#endif
    .combineFunc  = stdCombine,
    .pPartialFunc = "_std_partial",
    .pStateFunc = "_std_state",
    .pMergeFunc   = "_stddev_merge"
  },
  {
    .name = "_std_partial",
    .type = FUNCTION_TYPE_STD_PARTIAL,
    .classification = FUNC_MGT_AGG_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateStdPartial,
    .getEnvFunc   = getStdFuncEnv,
    .initFunc     = stdFunctionSetup,
    .processFunc  = stdFunction,
    .finalizeFunc = stdPartialFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = stdInvertFunction,
#endif
    .combineFunc  = stdCombine,
  },
  {
    .name = "_stddev_merge",
    .type = FUNCTION_TYPE_STDDEV_MERGE,
    .classification = FUNC_MGT_AGG_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateStdMerge,
    .getEnvFunc   = getStdFuncEnv,
    .initFunc     = stdFunctionSetup,
    .processFunc  = stdFunctionMerge,
    .finalizeFunc = stddevFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = stdInvertFunction,
#endif
    .combineFunc  = stdCombine,
    .pPartialFunc = "_std_state_merge",
    .pMergeFunc = "_stddev_merge",
  },
  {
    .name = "leastsquares",
    .type = FUNCTION_TYPE_LEASTSQUARES,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_FORBID_STREAM_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC,
    .parameters = {.minParamNum = 3,
                   .maxParamNum = 3,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateLeastSQR,
    .getEnvFunc   = getLeastSQRFuncEnv,
    .initFunc     = leastSQRFunctionSetup,
    .processFunc  = leastSQRFunction,
    .sprocessFunc = leastSQRScalarFunction,
    .finalizeFunc = leastSQRFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc  = leastSQRCombine,
  },
  {
    .name = "avg",
    .type = FUNCTION_TYPE_AVG,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SPECIAL_DATA_REQUIRED | FUNC_MGT_IGNORE_NULL_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .dataRequiredFunc = statisDataRequired,
    .getEnvFunc   = getAvgFuncEnv,
    .initFunc     = avgFunctionSetup,
    .processFunc  = avgFunction,
    .sprocessFunc = avgScalarFunction,
    .finalizeFunc = avgFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = avgInvertFunction,
#endif
    .combineFunc  = avgCombine,
    .pPartialFunc = "_avg_partial",
    .pMiddleFunc  = "_avg_middle",
    .pMergeFunc   = "_avg_merge",
    .pStateFunc = "_avg_state",
  },
  {
    .name = "_avg_partial",
    .type = FUNCTION_TYPE_AVG_PARTIAL,
    .classification = FUNC_MGT_AGG_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateAvgPartial,
    .dataRequiredFunc = statisDataRequired,
    .getEnvFunc   = getAvgFuncEnv,
    .initFunc     = avgFunctionSetup,
    .processFunc  = avgFunction,
    .finalizeFunc = avgPartialFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = avgInvertFunction,
#endif
    .combineFunc  = avgCombine,
  },
  {
    .name = "_avg_merge",
    .type = FUNCTION_TYPE_AVG_MERGE,
    .classification = FUNC_MGT_AGG_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateAvgMerge,
    .getEnvFunc   = getAvgFuncEnv,
    .initFunc     = avgFunctionSetup,
    .processFunc  = avgFunctionMerge,
    .finalizeFunc = avgFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = avgInvertFunction,
#endif
    .combineFunc  = avgCombine,
    .pPartialFunc = "_avg_state_merge",
    .pMergeFunc = "_avg_merge",
  },
  {
    .name = "percentile",
    .type = FUNCTION_TYPE_PERCENTILE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_REPEAT_SCAN_FUNC | FUNC_MGT_SPECIAL_DATA_REQUIRED | FUNC_MGT_FORBID_STREAM_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 11,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 11,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = true,
                                           .range = {.dMinVal = 0.0, .dMaxVal = 100.0}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translatePercentile,
    .dataRequiredFunc = statisDataRequired,
    .getEnvFunc   = getPercentileFuncEnv,
    .initFunc     = percentileFunctionSetup,
    .processFunc  = percentileFunction,
    .sprocessFunc = percentileScalarFunction,
    .finalizeFunc = percentileFinalize,
    .cleanupFunc  = percentileFunctionCleanupExt,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc  = NULL,
  },
  {
    .name = "apercentile",
    .type = FUNCTION_TYPE_APERCENTILE,
    .classification = FUNC_MGT_AGG_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 3,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = true,
                                           .range = {.dMinVal = 0.0, .dMaxVal = 100.0}},
                   .inputParaInfo[0][2] = {.isLastParam = true,
                                           .startParam = 3,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_STRING_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedValueSize = 2,
                                           .fixedValue = {"default", "t-digest"}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateApercentile,
    .getEnvFunc   = getApercentileFuncEnv,
    .initFunc     = apercentileFunctionSetup,
    .processFunc  = apercentileFunction,
    .sprocessFunc = apercentileScalarFunction,
    .finalizeFunc = apercentileFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc  = apercentileCombine,
    .pPartialFunc = "_apercentile_partial",
    .pMergeFunc   = "_apercentile_merge",
    .createMergeParaFuc = apercentileCreateMergeParam
  },
  {
    .name = "_apercentile_partial",
    .type = FUNCTION_TYPE_APERCENTILE_PARTIAL,
    .classification = FUNC_MGT_AGG_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 3,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = true,
                                           .range = {.dMinVal = 0.0, .dMaxVal = 100.0}},
                   .inputParaInfo[0][2] = {.isLastParam = true,
                                           .startParam = 3,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_STRING_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedValueSize = 2,
                                           .fixedValue = {"default", "t-digest"}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateApercentilePartial,
    .getEnvFunc   = getApercentileFuncEnv,
    .initFunc     = apercentileFunctionSetup,
    .processFunc  = apercentileFunction,
    .finalizeFunc = apercentilePartialFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc = apercentileCombine,
  },
  {
    .name = "_apercentile_merge",
    .type = FUNCTION_TYPE_APERCENTILE_MERGE,
    .classification = FUNC_MGT_AGG_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 3,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = true,
                                           .range = {.dMinVal = 0.0, .dMaxVal = 100.0}},
                   .inputParaInfo[0][2] = {.isLastParam = true,
                                           .startParam = 3,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_STRING_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedValueSize = 2,
                                           .fixedValue = {"default", "t-digest"}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateApercentileMerge,
    .getEnvFunc   = getApercentileFuncEnv,
    .initFunc     = apercentileFunctionSetup,
    .processFunc  = apercentileFunctionMerge,
    .finalizeFunc = apercentileFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc = apercentileCombine,
  },
  {
    .name = "top",
    .type = FUNCTION_TYPE_TOP,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_MULTI_ROWS_FUNC | FUNC_MGT_KEEP_ORDER_FUNC |
                      FUNC_MGT_FORBID_STREAM_FUNC | FUNC_MGT_FORBID_FILL_FUNC | FUNC_MGT_IGNORE_NULL_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = true,
                                           .range = {.dMinVal = 1.0, .dMaxVal = TOP_BOTTOM_QUERY_LIMIT}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE}},
    .translateFunc = translateTopBot,
    .getEnvFunc   = getTopBotFuncEnv,
    .initFunc     = topBotFunctionSetup,
    .processFunc  = topFunction,
    .sprocessFunc = topBotScalarFunction,
    .finalizeFunc = topBotFinalize,
    .combineFunc  = topCombine,
    .pPartialFunc = "top",
    .pMergeFunc   = "top",
    .createMergeParaFuc = topBotCreateMergeParam
  },
  {
    .name = "bottom",
    .type = FUNCTION_TYPE_BOTTOM,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_MULTI_ROWS_FUNC | FUNC_MGT_KEEP_ORDER_FUNC |
                      FUNC_MGT_FORBID_STREAM_FUNC | FUNC_MGT_FORBID_FILL_FUNC | FUNC_MGT_IGNORE_NULL_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = true,
                                           .range = {.dMinVal = 1.0, .dMaxVal = TOP_BOTTOM_QUERY_LIMIT}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE}},
    .translateFunc = translateTopBot,
    .getEnvFunc   = getTopBotFuncEnv,
    .initFunc     = topBotFunctionSetup,
    .processFunc  = bottomFunction,
    .sprocessFunc = topBotScalarFunction,
    .finalizeFunc = topBotFinalize,
    .combineFunc  = bottomCombine,
    .pPartialFunc = "bottom",
    .pMergeFunc   = "bottom",
    .createMergeParaFuc = topBotCreateMergeParam
  },
  {
    .name = "spread",
    .type = FUNCTION_TYPE_SPREAD,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SPECIAL_DATA_REQUIRED | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateSpread,
    .dataRequiredFunc = statisDataRequired,
    .getEnvFunc   = getSpreadFuncEnv,
    .initFunc     = spreadFunctionSetup,
    .processFunc  = spreadFunction,
    .sprocessFunc = spreadScalarFunction,
    .finalizeFunc = spreadFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc  = spreadCombine,
    .pPartialFunc = "_spread_partial",
    .pStateFunc = "_spread_state",
    .pMergeFunc   = "_spread_merge"
  },
  {
    .name = "_spread_partial",
    .type = FUNCTION_TYPE_SPREAD_PARTIAL,
    .classification = FUNC_MGT_AGG_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateSpreadPartial,
    .dataRequiredFunc = statisDataRequired,
    .getEnvFunc   = getSpreadFuncEnv,
    .initFunc     = spreadFunctionSetup,
    .processFunc  = spreadFunction,
    .finalizeFunc = spreadPartialFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc  = spreadCombine,
  },
  {
    .name = "_spread_merge",
    .type = FUNCTION_TYPE_SPREAD_MERGE,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .classification = FUNC_MGT_AGG_FUNC,
    .translateFunc = translateSpreadMerge,
    .dataRequiredFunc = statisDataRequired,
    .getEnvFunc   = getSpreadFuncEnv,
    .initFunc     = spreadFunctionSetup,
    .processFunc  = spreadFunctionMerge,
    .finalizeFunc = spreadFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc  = spreadCombine,
    .pPartialFunc = "_spread_state_merge",
    .pMergeFunc = "_spread_merge",
  },
  {
    .name = "elapsed",
    .type = FUNCTION_TYPE_ELAPSED,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_TIMELINE_FUNC | FUNC_MGT_INTERVAL_INTERPO_FUNC | FUNC_MGT_FORBID_STREAM_FUNC |
                      FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_SPECIAL_DATA_REQUIRED,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_COLUMN_NODE,
                                           .isPK = true,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedValueSize = 8,
                                           .fixedValue = {"1b", "1u", "1a", "1s", "1m", "1h", "1d", "1w"}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .dataRequiredFunc = statisDataRequired,
    .translateFunc = translateElapsed,
    .getEnvFunc   = getElapsedFuncEnv,
    .initFunc     = elapsedFunctionSetup,
    .processFunc  = elapsedFunction,
    .finalizeFunc = elapsedFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc  = elapsedCombine,
  },
  {
    .name = "_elapsed_partial",
    .type = FUNCTION_TYPE_ELAPSED,
    .classification = FUNC_MGT_AGG_FUNC,
    .parameters = {},
    .dataRequiredFunc = statisDataRequired,
    .translateFunc = translateElapsedPartial,
    .getEnvFunc   = getElapsedFuncEnv,
    .initFunc     = elapsedFunctionSetup,
    .processFunc  = elapsedFunction,
    .finalizeFunc = elapsedPartialFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc  = elapsedCombine,
  },
  {
    .name = "_elapsed_merge",
    .type = FUNCTION_TYPE_ELAPSED,
    .classification = FUNC_MGT_AGG_FUNC,
    .parameters = {},
    .dataRequiredFunc = statisDataRequired,
    .translateFunc = translateElapsedMerge,
    .getEnvFunc   = getElapsedFuncEnv,
    .initFunc     = elapsedFunctionSetup,
    .processFunc  = elapsedFunctionMerge,
    .finalizeFunc = elapsedFinalize,
  #ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
  #endif
    .combineFunc  = elapsedCombine,
  },
  {
    .name = "interp",
    .type = FUNCTION_TYPE_INTERP,
    .classification = FUNC_MGT_TIMELINE_FUNC | FUNC_MGT_INTERVAL_INTERPO_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_FORBID_STREAM_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_BOOL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_NOT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedValueSize = 2,
                                           .fixedNumValue = {0, 1}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE}},
    .translateFunc = translateInterp,
    .getEnvFunc    = getSelectivityFuncEnv,
    .initFunc      = functionSetup,
    .processFunc   = NULL,
    .finalizeFunc  = NULL,
    .estimateReturnRowsFunc = interpEstReturnRows
  },
  {
    .name = "derivative",
    .type = FUNCTION_TYPE_DERIVATIVE,
    .classification = FUNC_MGT_INDEFINITE_ROWS_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_TIMELINE_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_CUMULATIVE_FUNC | FUNC_MGT_FORBID_STREAM_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 3,
                   .maxParamNum = 3,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = true,
                                           .range = {.dMinVal = 1.0, .dMaxVal = DBL_MAX}},
                   .inputParaInfo[0][2] = {.isLastParam = true,
                                           .startParam = 3,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedValueSize = 2,
                                           .fixedNumValue = {0, 1}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateDerivative,
    .getEnvFunc   = getDerivativeFuncEnv,
    .initFunc     = derivativeFuncSetup,
    .processFunc  = derivativeFunction,
    .sprocessFunc = derivativeScalarFunction,
    .finalizeFunc = functionFinalize,
    .estimateReturnRowsFunc = derivativeEstReturnRows
  },
  {
    .name = "irate",
    .type = FUNCTION_TYPE_IRATE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_TIMELINE_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC | FUNC_MGT_FORBID_STREAM_FUNC |
                      FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateIrate,
    .getEnvFunc   = getIrateFuncEnv,
    .initFunc     = irateFuncSetup,
    .processFunc  = irateFunction,
    .sprocessFunc = irateScalarFunction,
    .finalizeFunc = irateFinalize,
    .pPartialFunc = "_irate_partial",
    .pMergeFunc   = "_irate_merge"
  },
  {
    .name = "_irate_partial",
    .type = FUNCTION_TYPE_IRATE_PARTIAL,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_TIMELINE_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC | FUNC_MGT_FORBID_STREAM_FUNC |
                      FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 3,
                   .maxParamNum = 4,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_TINYINT_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][2] = {.isLastParam = false,
                                           .startParam = 3,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_COLUMN_NODE,
                                           .isPK = false,
                                           .isTs = true,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][3] = {.isLastParam = true,
                                           .startParam = 4,
                                           .endParam = 4,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_COLUMN_NODE,
                                           .isPK = true,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateIratePartial,
    .getEnvFunc   = getIrateFuncEnv,
    .initFunc     = irateFuncSetup,
    .processFunc  = irateFunction,
    .sprocessFunc = irateScalarFunction,
    .finalizeFunc = iratePartialFinalize
  },
  {
    .name = "_irate_merge",
    .type = FUNCTION_TYPE_IRATE_MERGE,
    .classification = FUNC_MGT_AGG_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateIrateMerge,
    .getEnvFunc   = getIrateFuncEnv,
    .initFunc     = irateFuncSetup,
    .processFunc  = irateFunctionMerge,
    .sprocessFunc = irateScalarFunction,
    .finalizeFunc = irateFinalize
  },
  {
    .name = "last_row",
    .type = FUNCTION_TYPE_LAST_ROW,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_MULTI_RES_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = -1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = -1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_NOT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE}},
    .translateFunc = translateFirstLast,
    .dynDataRequiredFunc = lastDynDataReq,
    .getEnvFunc   = getFirstLastFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = lastRowFunction,
    .sprocessFunc = firstLastScalarFunction,
    .pPartialFunc = "_last_row_partial",
    .pMergeFunc   = "_last_row_merge",
    .finalizeFunc = firstLastFinalize,
    .combineFunc  = lastCombine
  },
  {
    .name = "_cache_last_row",
    .type = FUNCTION_TYPE_CACHE_LAST_ROW,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_MULTI_RES_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_FORBID_STREAM_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = -1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = -1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_NOT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE}},
    .translateFunc = translateFirstLast,
    .getEnvFunc   = getFirstLastFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = cachedLastRowFunction,
    .finalizeFunc = firstLastFinalize
  },
  {
    .name = "_cache_last",
    .type = FUNCTION_TYPE_CACHE_LAST,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_MULTI_RES_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_FORBID_STREAM_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_IGNORE_NULL_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = -1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = -1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_NOT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE}},
    .translateFunc = translateFirstLast,
    .getEnvFunc   = getFirstLastFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = lastFunctionMerge,
    .finalizeFunc = firstLastFinalize
  },
  {
    .name = "_last_row_partial",
    .type = FUNCTION_TYPE_LAST_PARTIAL,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_MULTI_RES_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = -1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = -1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_NOT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateFirstLastPartial,
    .dynDataRequiredFunc = lastDynDataReq,
    .getEnvFunc   = getFirstLastFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = lastRowFunction,
    .finalizeFunc = firstLastPartialFinalize,
  },
  {
    .name = "_last_row_merge",
    .type = FUNCTION_TYPE_LAST_MERGE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_MULTI_RES_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = -1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE}},
    .translateFunc = translateFirstLastMerge,
    .getEnvFunc   = getFirstLastFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = lastFunctionMerge,
    .finalizeFunc = firstLastFinalize,
  },
  {
    .name = "first",
    .type = FUNCTION_TYPE_FIRST,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_MULTI_RES_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_IGNORE_NULL_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = -1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = -1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_NOT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE}},
    .translateFunc = translateFirstLast,
    .dynDataRequiredFunc = firstDynDataReq,
    .getEnvFunc   = getFirstLastFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = firstFunction,
    .sprocessFunc = firstLastScalarFunction,
    .finalizeFunc = firstLastFinalize,
    .pPartialFunc = "_first_partial",
    .pStateFunc = "_first_state",
    .pMergeFunc   = "_first_merge",
    .combineFunc  = firstCombine,
  },
  {
    .name = "_first_partial",
    .type = FUNCTION_TYPE_FIRST_PARTIAL,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_MULTI_RES_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_IGNORE_NULL_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = -1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = -1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_NOT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateFirstLastPartial,
    .dynDataRequiredFunc = firstDynDataReq,
    .getEnvFunc   = getFirstLastFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = firstFunction,
    .finalizeFunc = firstLastPartialFinalize,
    .combineFunc  = firstCombine,
  },
  {
    .name = "_first_merge",
    .type = FUNCTION_TYPE_FIRST_MERGE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_MULTI_RES_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_IGNORE_NULL_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = -1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE}},
    .translateFunc = translateFirstLastMerge,
    .getEnvFunc   = getFirstLastFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = firstFunctionMerge,
    .finalizeFunc = firstLastFinalize,
    .combineFunc  = firstCombine,
    .pPartialFunc = "_first_state_merge",
    .pMergeFunc = "_first_merge",
  },
  {
    .name = "last",
    .type = FUNCTION_TYPE_LAST,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_MULTI_RES_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_IGNORE_NULL_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = -1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = -1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_NOT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE}},
    .translateFunc = translateFirstLast,
    .dynDataRequiredFunc = lastDynDataReq,
    .getEnvFunc   = getFirstLastFuncEnv,
    .initFunc     = firstLastFunctionSetup,
    .processFunc  = lastFunction,
    .sprocessFunc = firstLastScalarFunction,
    .finalizeFunc = firstLastFinalize,
    .pPartialFunc = "_last_partial",
    .pStateFunc = "_last_state",
    .pMergeFunc   = "_last_merge",
    .combineFunc  = lastCombine,
  },
  {
    .name = "_last_partial",
    .type = FUNCTION_TYPE_LAST_PARTIAL,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_MULTI_RES_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_IGNORE_NULL_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = -1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = -1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_NOT_VALUE_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateFirstLastPartial,
    .dynDataRequiredFunc = lastDynDataReq,
    .getEnvFunc   = getFirstLastFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = lastFunction,
    .finalizeFunc = firstLastPartialFinalize,
    .combineFunc  = lastCombine,
  },
  {
    .name = "_last_merge",
    .type = FUNCTION_TYPE_LAST_MERGE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_MULTI_RES_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_IGNORE_NULL_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = -1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = -1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE}},
    .translateFunc = translateFirstLastMerge,
    .getEnvFunc   = getFirstLastFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = lastFunctionMerge,
    .finalizeFunc = firstLastFinalize,
    .combineFunc  = lastCombine,
    .pPartialFunc = "_last_state_merge",
    .pMergeFunc = "_last_merge",
  },
  {
    .name = "twa",
    .type = FUNCTION_TYPE_TWA,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_TIMELINE_FUNC | FUNC_MGT_INTERVAL_INTERPO_FUNC | FUNC_MGT_FORBID_STREAM_FUNC |
                      FUNC_MGT_IMPLICIT_TS_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .dataRequiredFunc = statisDataRequired,
    .getEnvFunc    = getTwaFuncEnv,
    .initFunc      = twaFunctionSetup,
    .processFunc   = twaFunction,
    .sprocessFunc  = twaScalarFunction,
    .finalizeFunc  = twaFinalize
  },
  {
    .name = "histogram",
    .type = FUNCTION_TYPE_HISTOGRAM,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_MULTI_ROWS_FUNC | FUNC_MGT_FORBID_FILL_FUNC | FUNC_MGT_FORBID_STREAM_FUNC,
    .parameters = {.minParamNum = 4,
                   .maxParamNum = 4,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedValue = {"user_input", "linear_bin", "log_bin"}},
                   .inputParaInfo[0][2] = {.isLastParam = false,
                                           .startParam = 3,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][3] = {.isLastParam = true,
                                           .startParam = 4,
                                           .endParam = 4,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedNumValue = {0, 1}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateHistogram,
    .getEnvFunc   = getHistogramFuncEnv,
    .initFunc     = histogramFunctionSetup,
    .processFunc  = histogramFunction,
    .sprocessFunc = histogramScalarFunction,
    .finalizeFunc = histogramFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc  = histogramCombine,
    .pPartialFunc = "_histogram_partial",
    .pMergeFunc   = "_histogram_merge",
  },
  {
    .name = "_histogram_partial",
    .type = FUNCTION_TYPE_HISTOGRAM_PARTIAL,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_MULTI_ROWS_FUNC | FUNC_MGT_FORBID_FILL_FUNC,
    .parameters = {.minParamNum = 4,
                   .maxParamNum = 4,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedValue = {"user_input", "linear_bin", "log_bin"}},
                   .inputParaInfo[0][2] = {.isLastParam = false,
                                           .startParam = 3,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][3] = {.isLastParam = true,
                                           .startParam = 4,
                                           .endParam = 4,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedNumValue = {0, 1}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateHistogramPartial,
    .getEnvFunc   = getHistogramFuncEnv,
    .initFunc     = histogramFunctionSetup,
    .processFunc  = histogramFunctionPartial,
    .finalizeFunc = histogramPartialFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc  = histogramCombine,
  },
  {
    .name = "_histogram_merge",
    .type = FUNCTION_TYPE_HISTOGRAM_MERGE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_MULTI_ROWS_FUNC | FUNC_MGT_FORBID_FILL_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateHistogramMerge,
    .getEnvFunc   = getHistogramFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = histogramFunctionMerge,
    .finalizeFunc = histogramFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc  = histogramCombine,
  },
  {
    .name = "hyperloglog",
    .type = FUNCTION_TYPE_HYPERLOGLOG,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_COUNT_LIKE_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateHLL,
    .getEnvFunc   = getHLLFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = hllFunction,
    .sprocessFunc = hllScalarFunction,
    .finalizeFunc = hllFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc  = hllCombine,
    .pPartialFunc = "_hyperloglog_partial",
    .pMergeFunc   = "_hyperloglog_merge"
  },
  {
    .name = "_hyperloglog_partial",
    .type = FUNCTION_TYPE_HYPERLOGLOG_PARTIAL,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .classification = FUNC_MGT_AGG_FUNC,
    .translateFunc = translateHLLPartial,
    .getEnvFunc   = getHLLFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = hllFunction,
    .finalizeFunc = hllPartialFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc  = hllCombine,
  },
  {
    .name = "_hyperloglog_merge",
    .type = FUNCTION_TYPE_HYPERLOGLOG_MERGE,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .classification = FUNC_MGT_AGG_FUNC,
    .translateFunc = translateHLLMerge,
    .getEnvFunc   = getHLLFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = hllFunctionMerge,
    .finalizeFunc = hllFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = NULL,
#endif
    .combineFunc  = hllCombine,
    .pMergeFunc = "_hyperloglog_merge",
  },
  {
    .name = "diff",
    .type = FUNCTION_TYPE_DIFF,
    .classification = FUNC_MGT_INDEFINITE_ROWS_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_TIMELINE_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC | FUNC_MGT_PROCESS_BY_ROW |
                      FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_FORBID_STREAM_FUNC | FUNC_MGT_CUMULATIVE_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE | FUNC_PARAM_SUPPORT_BOOL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedValueSize = 4,
                                           .fixedNumValue = {0, 1, 2, 3}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE | FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateDiff,
    .getEnvFunc   = getDiffFuncEnv,
    .initFunc     = diffFunctionSetup,
    .processFunc  = diffFunction,
    .sprocessFunc = diffScalarFunction,
    .finalizeFunc = functionFinalize,
    .estimateReturnRowsFunc = diffEstReturnRows,
    .processFuncByRow  = diffFunctionByRow,
  },
  {
    .name = "statecount",
    .type = FUNCTION_TYPE_STATE_COUNT,
    .classification = FUNC_MGT_INDEFINITE_ROWS_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_TIMELINE_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_FORBID_STREAM_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC,
    .parameters = {.minParamNum = 3,
                   .maxParamNum = 3,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_STRING_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedValueSize = 6,
                                           .fixedValue = {"LT", "GT", "LE", "GE", "NE", "EQ"}},
                   .inputParaInfo[0][2] = {.isLastParam = true,
                                           .startParam = 3,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE | FUNC_PARAM_SUPPORT_BIGINT_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateStateCount,
    .getEnvFunc   = getStateFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = stateCountFunction,
    .sprocessFunc = stateCountScalarFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "stateduration",
    .type = FUNCTION_TYPE_STATE_DURATION,
    .classification = FUNC_MGT_INDEFINITE_ROWS_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_TIMELINE_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_FORBID_STREAM_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC,
    .parameters = {.minParamNum = 4,
                   .maxParamNum = 4,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_STRING_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedValueSize = 6,
                                           .fixedValue = {"LT", "GT", "LE", "GE", "NE", "EQ"}},
                   .inputParaInfo[0][2] = {.isLastParam = false,
                                           .startParam = 3,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE | FUNC_PARAM_SUPPORT_BIGINT_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][3] = {.isLastParam = true,
                                           .startParam = 4,
                                           .endParam = 4,
                                           .validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateStateDuration,
    .getEnvFunc   = getStateFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = stateDurationFunction,
    .sprocessFunc = stateDurationScalarFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "csum",
    .type = FUNCTION_TYPE_CSUM,
    .classification = FUNC_MGT_INDEFINITE_ROWS_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_TIMELINE_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_FORBID_STREAM_FUNC | FUNC_MGT_CUMULATIVE_FUNC | FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE | FUNC_PARAM_SUPPORT_DOUBLE_TYPE | FUNC_PARAM_SUPPORT_UBIGINT_TYPE}},
    .translateFunc = translateCsum,
    .getEnvFunc   = getCsumFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = csumFunction,
    .sprocessFunc = csumScalarFunction,
    .finalizeFunc = NULL,
    .estimateReturnRowsFunc = csumEstReturnRows,
  },
  {
    .name = "mavg",
    .type = FUNCTION_TYPE_MAVG,
    .classification = FUNC_MGT_INDEFINITE_ROWS_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_TIMELINE_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_FORBID_STREAM_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = true,
                                           .range = {.dMinVal = 1.0, .dMaxVal = 100.0}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateMavg,
    .getEnvFunc   = getMavgFuncEnv,
    .initFunc     = mavgFunctionSetup,
    .processFunc  = mavgFunction,
    .sprocessFunc = mavgScalarFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "sample",
    .type = FUNCTION_TYPE_SAMPLE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_MULTI_ROWS_FUNC | FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_FORBID_STREAM_FUNC |
                      FUNC_MGT_FORBID_FILL_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = true,
                                           .range = {.dMinVal = 1.0, .dMaxVal = 100.0}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE}},
    .translateFunc = translateSample,
    .getEnvFunc   = getSampleFuncEnv,
    .initFunc     = sampleFunctionSetup,
    .processFunc  = sampleFunction,
    .sprocessFunc = sampleScalarFunction,
    .finalizeFunc = sampleFinalize
  },
  {
    .name = "tail",
    .type = FUNCTION_TYPE_TAIL,
    .classification = FUNC_MGT_SELECT_FUNC | FUNC_MGT_INDEFINITE_ROWS_FUNC | FUNC_MGT_FORBID_STREAM_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 3,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = true,
                                           .range = {.dMinVal = 1.0, .dMaxVal = 100.0}},
                   .inputParaInfo[0][2] = {.isLastParam = true,
                                           .startParam = 3,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = true,
                                           .range = {.dMinVal = 0.0, .dMaxVal = 100.0}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE}},
    .translateFunc = translateTail,
    .getEnvFunc   = getTailFuncEnv,
    .initFunc     = tailFunctionSetup,
    .processFunc  = tailFunction,
    .sprocessFunc = tailScalarFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "unique",
    .type = FUNCTION_TYPE_UNIQUE,
    .classification = FUNC_MGT_SELECT_FUNC | FUNC_MGT_INDEFINITE_ROWS_FUNC | FUNC_MGT_FORBID_STREAM_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false,
                                           .hasColumn = true},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE}},
    .translateFunc = translateUnique,
    .getEnvFunc   = getUniqueFuncEnv,
    .initFunc     = uniqueFunctionSetup,
    .processFunc  = uniqueFunction,
    .sprocessFunc = uniqueScalarFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "mode",
    .type = FUNCTION_TYPE_MODE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_FORBID_STREAM_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false,
                                           .hasColumn = true},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE}},
    .translateFunc = translateMode,
    .getEnvFunc   = getModeFuncEnv,
    .initFunc     = modeFunctionSetup,
    .processFunc  = modeFunction,
    .sprocessFunc = modeScalarFunction,
    .finalizeFunc = modeFinalize,
    .cleanupFunc  = modeFunctionCleanupExt
  },
  {
    .name = "abs",
    .type = FUNCTION_TYPE_ABS,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE}},
    .translateFunc = translateInOutNum,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = absFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "log",
    .type = FUNCTION_TYPE_LOG,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateLogarithm,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = logFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "pow",
    .type = FUNCTION_TYPE_POW,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateIn2NumOutDou,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = powFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "sqrt",
    .type = FUNCTION_TYPE_SQRT,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = sqrtFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "ceil",
    .type = FUNCTION_TYPE_CEIL,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE}},
    .translateFunc = translateInOutNum,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = ceilFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "floor",
    .type = FUNCTION_TYPE_FLOOR,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE}},
    .translateFunc = translateInOutNum,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = floorFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "round",
    .type = FUNCTION_TYPE_ROUND,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateRound,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = roundFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "sin",
    .type = FUNCTION_TYPE_SIN,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = sinFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "cos",
    .type = FUNCTION_TYPE_COS,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = cosFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "tan",
    .type = FUNCTION_TYPE_TAN,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = tanFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "asin",
    .type = FUNCTION_TYPE_ASIN,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = asinFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "acos",
    .type = FUNCTION_TYPE_ACOS,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = acosFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "atan",
    .type = FUNCTION_TYPE_ATAN,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = atanFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "length",
    .type = FUNCTION_TYPE_LENGTH,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_STRING_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateLength,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = lengthFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "char_length",
    .type = FUNCTION_TYPE_CHAR_LENGTH,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateCharLength,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = charLengthFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "concat",
    .type = FUNCTION_TYPE_CONCAT,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 8,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 8,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE}},
    .translateFunc = translateConcat,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = concatFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "concat_ws",
    .type = FUNCTION_TYPE_CONCAT_WS,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 3,
                   .maxParamNum = 9,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 9,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE}},
    .translateFunc = translateConcatWs,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = concatWsFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "lower",
    .type = FUNCTION_TYPE_LOWER,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE}},
    .translateFunc = translateInOutStr,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = lowerFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "upper",
    .type = FUNCTION_TYPE_UPPER,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE}},
    .translateFunc = translateInOutStr,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = upperFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "ltrim",
    .type = FUNCTION_TYPE_LTRIM,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE}},
    .translateFunc = translateLtrim,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = ltrimFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "rtrim",
    .type = FUNCTION_TYPE_RTRIM,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE}},
    .translateFunc = translateRtrim,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = rtrimFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "substr",
    .type = FUNCTION_TYPE_SUBSTR,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 3,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE}},
    .translateFunc = translateSubstr,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = substrFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "cast",
    .type = FUNCTION_TYPE_CAST,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE,// TODO(smj) : without varbinary and json
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE}},
    .translateFunc = translateCast,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = castFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "to_iso8601",
    .type = FUNCTION_TYPE_TO_ISO8601,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE | FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateToIso8601,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = toISO8601Function,
    .finalizeFunc = NULL
  },
  {
    .name = "to_unixtimestamp",
    .type = FUNCTION_TYPE_TO_UNIXTIMESTAMP,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedValueSize = 2,
                                           .fixedValue = {0, 1}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE | FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE}},
    .translateFunc = translateToUnixtimestamp,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = toUnixtimestampFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "timetruncate",
    .type = FUNCTION_TYPE_TIMETRUNCATE,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 3,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE | FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][2] = {.isLastParam = true,
                                           .startParam = 3,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedValueSize = 2,
                                           .fixedNumValue = {0, 1}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE}},
    .translateFunc = translateTimeTruncate,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = timeTruncateFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "timediff",
    .type = FUNCTION_TYPE_TIMEDIFF,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 3,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE | FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 3,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE}},
    .translateFunc = translateTimeDiff,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = timeDiffFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "now",
    .type = FUNCTION_TYPE_NOW,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_DATETIME_FUNC | FUNC_MGT_KEEP_ORDER_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE}},
    .translateFunc = translateNowToday,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = nowFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "today",
    .type = FUNCTION_TYPE_TODAY,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_DATETIME_FUNC | FUNC_MGT_KEEP_ORDER_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE}},
    .translateFunc = translateNowToday,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = todayFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "timezone",
    .type = FUNCTION_TYPE_TIMEZONE,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateTimezone,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = timezoneFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "tbname",
    .type = FUNCTION_TYPE_TBNAME,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_SCAN_PC_FUNC | FUNC_MGT_KEEP_ORDER_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateTbnameColumn,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = qPseudoTagFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "_qstart",
    .type = FUNCTION_TYPE_QSTART,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_CLIENT_PC_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE}},
    .translateFunc = translateTimePseudoColumn,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = NULL,
    .finalizeFunc = NULL
  },
  {
    .name = "_qend",
    .type = FUNCTION_TYPE_QEND,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_CLIENT_PC_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE}},
    .translateFunc = translateTimePseudoColumn,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = NULL,
    .finalizeFunc = NULL
  },
  {
    .name = "_qduration",
    .type = FUNCTION_TYPE_QDURATION,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_CLIENT_PC_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateWduration,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = NULL,
    .finalizeFunc = NULL
  },
  {
    .name = "_wstart",
    .type = FUNCTION_TYPE_WSTART,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_WINDOW_PC_FUNC | FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_SKIP_SCAN_CHECK_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE}},
    .translateFunc = translateTimePseudoColumn,
    .getEnvFunc   = getTimePseudoFuncEnv,
    .initFunc     = NULL,
    .sprocessFunc = winStartTsFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "_wend",
    .type = FUNCTION_TYPE_WEND,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_WINDOW_PC_FUNC | FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_SKIP_SCAN_CHECK_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE}},
    .translateFunc = translateTimePseudoColumn,
    .getEnvFunc   = getTimePseudoFuncEnv,
    .initFunc     = NULL,
    .sprocessFunc = winEndTsFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "_wduration",
    .type = FUNCTION_TYPE_WDURATION,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_WINDOW_PC_FUNC | FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_SKIP_SCAN_CHECK_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateWduration,
    .getEnvFunc   = getTimePseudoFuncEnv,
    .initFunc     = NULL,
    .sprocessFunc = winDurFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "to_json",
    .type = FUNCTION_TYPE_TO_JSON,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_VALUE_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_JSON_TYPE}},
    .translateFunc = translateToJson,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = toJsonFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "_select_value",
    .type = FUNCTION_TYPE_SELECT_VALUE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_KEEP_ORDER_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE}},
    .translateFunc = translateSelectValue,
    .getEnvFunc   = getSelectivityFuncEnv,  // todo remove this function later.
    .initFunc     = functionSetup,
    .processFunc  = NULL,
    .finalizeFunc = NULL,
    .pPartialFunc = "_select_value",
    .pMergeFunc   = "_select_value"
  },
  {
    .name = "_block_dist",
    .type = FUNCTION_TYPE_BLOCK_DIST,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_FORBID_STREAM_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateBlockDistFunc,
    .getEnvFunc   = getBlockDistFuncEnv,
    .initFunc     = blockDistSetup,
    .processFunc  = blockDistFunction,
    .finalizeFunc = blockDistFinalize
  },
  {
    .name = "_block_dist_info",
    .type = FUNCTION_TYPE_BLOCK_DIST_INFO,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_SCAN_PC_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateBlockDistInfoFunc,
  },
  {
    .name = "_group_key",
    .type = FUNCTION_TYPE_GROUP_KEY,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_SKIP_SCAN_CHECK_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE}},
    .translateFunc = translateGroupKey,
    .getEnvFunc   = getGroupKeyFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = groupKeyFunction,
    .finalizeFunc = groupKeyFinalize,
    .combineFunc  = groupKeyCombine,
    .pPartialFunc = "_group_key",
    .pMergeFunc   = "_group_key"
  },
  {
    .name = "database",
    .type = FUNCTION_TYPE_DATABASE,
    .classification = FUNC_MGT_SYSTEM_INFO_FUNC | FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateDatabaseFunc,
  },
  {
    .name = "client_version",
    .type = FUNCTION_TYPE_CLIENT_VERSION,
    .classification = FUNC_MGT_SYSTEM_INFO_FUNC | FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateClientVersionFunc,
  },
  {
    .name = "server_version",
    .type = FUNCTION_TYPE_SERVER_VERSION,
    .classification = FUNC_MGT_SYSTEM_INFO_FUNC | FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateServerVersionFunc,
  },
  {
    .name = "server_status",
    .type = FUNCTION_TYPE_SERVER_STATUS,
    .classification = FUNC_MGT_SYSTEM_INFO_FUNC | FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateServerStatusFunc,
  },
  {
    .name = "current_user",
    .type = FUNCTION_TYPE_CURRENT_USER,
    .classification = FUNC_MGT_SYSTEM_INFO_FUNC | FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateCurrentUserFunc,
  },
  {
    .name = "user",
    .type = FUNCTION_TYPE_USER,
    .classification = FUNC_MGT_SYSTEM_INFO_FUNC | FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateUserFunc,
  },
  {
    .name = "_irowts",
    .type = FUNCTION_TYPE_IROWTS,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_INTERP_PC_FUNC | FUNC_MGT_KEEP_ORDER_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE}},
    .translateFunc = translateTimePseudoColumn,
    .getEnvFunc   = getTimePseudoFuncEnv,
    .initFunc     = NULL,
    .sprocessFunc = NULL,
    .finalizeFunc = NULL
  },
  {
    .name = "_isfilled",
    .type = FUNCTION_TYPE_ISFILLED,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_INTERP_PC_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BOOL_TYPE}},
    .translateFunc = translateIsFilledPseudoColumn,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = NULL,
    .finalizeFunc = NULL
  },
  {
    .name = "_tags",
    .type = FUNCTION_TYPE_TAGS,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_MULTI_RES_FUNC,
    .parameters = {},
    .translateFunc = translateTagsPseudoColumn,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = NULL,
    .finalizeFunc = NULL
  },
  {
    .name = "_table_count",
    .type = FUNCTION_TYPE_TABLE_COUNT,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_SCAN_PC_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateTableCountPseudoColumn,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = NULL,
    .finalizeFunc = NULL
  },
  {
    .name = "st_geomfromtext",
    .type = FUNCTION_TYPE_GEOM_FROM_TEXT,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_GEOMETRY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_STRING_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_GEOMETRY_TYPE}},
    .translateFunc = translateInStrOutGeom,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = geomFromTextFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "st_astext",
    .type = FUNCTION_TYPE_AS_TEXT,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_GEOMETRY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_GEOMETRY_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_STRING_TYPE}},
    .translateFunc = translateInGeomOutStr,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = asTextFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "st_makepoint",
    .type = FUNCTION_TYPE_MAKE_POINT,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_GEOMETRY_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_GEOMETRY_TYPE}},
    .translateFunc = translateIn2NumOutGeom,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = makePointFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "st_intersects",
    .type = FUNCTION_TYPE_INTERSECTS,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_GEOMETRY_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_GEOMETRY_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BOOL_TYPE}},
    .translateFunc = translateIn2GeomOutBool,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = intersectsFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "st_equals",
    .type = FUNCTION_TYPE_EQUALS,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_GEOMETRY_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_GEOMETRY_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BOOL_TYPE}},
    .translateFunc = translateIn2GeomOutBool,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = equalsFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "st_touches",
    .type = FUNCTION_TYPE_TOUCHES,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_GEOMETRY_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_GEOMETRY_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BOOL_TYPE}},
    .translateFunc = translateIn2GeomOutBool,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = touchesFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "st_covers",
    .type = FUNCTION_TYPE_COVERS,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_GEOMETRY_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_GEOMETRY_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BOOL_TYPE}},
    .translateFunc = translateIn2GeomOutBool,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = coversFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "st_contains",
    .type = FUNCTION_TYPE_CONTAINS,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_GEOMETRY_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_GEOMETRY_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BOOL_TYPE}},
    .translateFunc = translateIn2GeomOutBool,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = containsFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "st_containsproperly",
    .type = FUNCTION_TYPE_CONTAINS_PROPERLY,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_GEOMETRY_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_GEOMETRY_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BOOL_TYPE}},
    .translateFunc = translateIn2GeomOutBool,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = containsProperlyFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "_tbuid",
    .type = FUNCTION_TYPE_TBUID,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_SCAN_PC_FUNC | FUNC_MGT_KEEP_ORDER_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateTbUidColumn,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = qPseudoTagFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "_vgid",
    .type = FUNCTION_TYPE_VGID,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_SCAN_PC_FUNC | FUNC_MGT_KEEP_ORDER_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_INT_TYPE}},
    .translateFunc = translateVgIdColumn,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = qPseudoTagFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "to_timestamp",
    .type = FUNCTION_TYPE_TO_TIMESTAMP,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_DATETIME_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_STRING_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE}},
    .translateFunc = translateToTimestamp,
    .getEnvFunc = NULL,
    .initFunc = NULL,
    .sprocessFunc = toTimestampFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "to_char",
    .type = FUNCTION_TYPE_TO_CHAR,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_STRING_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateToChar,
    .getEnvFunc = NULL,
    .initFunc = NULL,
    .sprocessFunc = toCharFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "_avg_middle",
    .type = FUNCTION_TYPE_AVG_PARTIAL,
    .classification = FUNC_MGT_AGG_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateAvgMiddle,
    .dataRequiredFunc = statisDataRequired,
    .getEnvFunc   = getAvgFuncEnv,
    .initFunc     = avgFunctionSetup,
    .processFunc  = avgFunctionMerge,
    .finalizeFunc = avgPartialFinalize,
#ifdef BUILD_NO_CALL
    .invertFunc   = avgInvertFunction,
#endif
    .combineFunc  = avgCombine,
  },
  {
    .name = "_vgver",
    .type = FUNCTION_TYPE_VGVER,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_SCAN_PC_FUNC | FUNC_MGT_KEEP_ORDER_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateVgVerColumn,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = qPseudoTagFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "_std_state",
    .type = FUNCTION_TYPE_STD_STATE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateStdState,
    .getEnvFunc   = getStdFuncEnv,
    .initFunc     = stdFunctionSetup,
    .processFunc  = stdFunction,
    .finalizeFunc = stdPartialFinalize,
    .pPartialFunc = "_std_partial",
    .pMergeFunc   = "_std_state_merge",
  },
  {
    .name = "_std_state_merge",
    .type = FUNCTION_TYPE_STD_STATE_MERGE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateStdStateMerge,
    .getEnvFunc = getStdFuncEnv,
    .initFunc = stdFunctionSetup,
    .processFunc = stdFunctionMerge,
    .finalizeFunc = stdPartialFinalize,
  },
  {
    .name = "_avg_state",
    .type = FUNCTION_TYPE_AVG_STATE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateAvgState,
    .getEnvFunc = getAvgFuncEnv,
    .initFunc = avgFunctionSetup,
    .processFunc = avgFunction,
    .finalizeFunc = avgPartialFinalize,
    .pPartialFunc = "_avg_partial",
    .pMergeFunc = "_avg_state_merge"
  },
  {
    .name = "_avg_state_merge",
    .type = FUNCTION_TYPE_AVG_STATE_MERGE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateAvgStateMerge,
    .getEnvFunc = getAvgFuncEnv,
    .initFunc = avgFunctionSetup,
    .processFunc = avgFunctionMerge,
    .finalizeFunc = avgPartialFinalize,
  },
  {
    .name = "_spread_state",
    .type = FUNCTION_TYPE_SPREAD_STATE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SPECIAL_DATA_REQUIRED | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_TIMESTAMP_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateSpreadState,
    .getEnvFunc = getSpreadFuncEnv,
    .initFunc = spreadFunctionSetup,
    .processFunc = spreadFunction,
    .finalizeFunc = spreadPartialFinalize,
    .pPartialFunc = "_spread_partial",
    .pMergeFunc = "_spread_state_merge"
  },
  {
    .name = "_spread_state_merge",
    .type = FUNCTION_TYPE_SPREAD_STATE_MERGE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateSpreadStateMerge,
    .getEnvFunc = getSpreadFuncEnv,
    .initFunc = spreadFunctionSetup,
    .processFunc = spreadFunctionMerge,
    .finalizeFunc = spreadPartialFinalize,
  },
  {
    .name = "_first_state",
    .type = FUNCTION_TYPE_FIRST_STATE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_MULTI_RES_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_TSMA_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = -1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = -1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateFirstLastState,
    .getEnvFunc = getFirstLastFuncEnv,
    .initFunc = functionSetup,
    .processFunc = firstFunction,
    .finalizeFunc = firstLastPartialFinalize,
    .pPartialFunc = "_first_partial",
    .pMergeFunc = "_first_state_merge"
  },
  {
    .name = "_first_state_merge",
    .type = FUNCTION_TYPE_FIRST_STATE_MERGE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_MULTI_RES_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_TSMA_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateFirstLastStateMerge,
    .getEnvFunc = getFirstLastFuncEnv,
    .initFunc = functionSetup,
    .processFunc = firstFunctionMerge,
    .finalizeFunc = firstLastPartialFinalize,
  },
  {
    .name = "_last_state",
    .type = FUNCTION_TYPE_LAST_STATE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_MULTI_RES_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_TSMA_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = -1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = -1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NOT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateFirstLastState,
    .getEnvFunc = getFirstLastFuncEnv,
    .initFunc = functionSetup,
    .processFunc = lastFunction,
    .finalizeFunc = firstLastPartialFinalize,
    .pPartialFunc = "_last_partial",
    .pMergeFunc = "_last_state_merge"
  },
  {
    .name = "_last_state_merge",
    .type = FUNCTION_TYPE_LAST_STATE_MERGE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_MULTI_RES_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_TSMA_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateFirstLastStateMerge,
    .getEnvFunc = getFirstLastFuncEnv,
    .initFunc = functionSetup,
    .processFunc = lastFunctionMerge,
    .finalizeFunc = firstLastPartialFinalize,
  },
  {
    .name = "_hyperloglog_state",
    .type = FUNCTION_TYPE_HYPERLOGLOG_STATE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_COUNT_LIKE_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateHLLState,
    .getEnvFunc = getHLLFuncEnv,
    .initFunc = functionSetup,
    .processFunc = hllFunction,
    .finalizeFunc = hllPartialFinalize,
    .pPartialFunc = "_hyperloglog_partial",
    .pMergeFunc = "_hyperloglog_state_merge",
  },
  {
    .name = "_hyperloglog_state_merge",
    .type = FUNCTION_TYPE_HYPERLOGLOG_STATE_MERGE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_COUNT_LIKE_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateHLLStateMerge,
    .getEnvFunc = getHLLFuncEnv,
    .initFunc = functionSetup,
    .processFunc = hllFunctionMerge,
    .finalizeFunc = hllPartialFinalize,
  },
  {
    .name = "md5",
    .type = FUNCTION_TYPE_MD5,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateMd5,
    .getEnvFunc = NULL,
    .initFunc = NULL,
    .sprocessFunc = md5Function,
    .finalizeFunc = NULL
  },
  {
    .name = "_group_const_value",
    .type = FUNCTION_TYPE_GROUP_CONST_VALUE,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_SELECT_FUNC | FUNC_MGT_KEEP_ORDER_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_ALL_TYPE}},
    .translateFunc = translateSelectValue,
    .getEnvFunc   = getSelectivityFuncEnv,
    .initFunc     = functionSetup,
    .processFunc  = groupConstValueFunction,
    .finalizeFunc = groupConstValueFinalize,
  },
  {
    .name = "stddev_pop",
    .type = FUNCTION_TYPE_STDDEV,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .getEnvFunc   = getStdFuncEnv,
    .initFunc     = stdFunctionSetup,
    .processFunc  = stdFunction,
    .sprocessFunc = stdScalarFunction,
    .finalizeFunc = stddevFinalize,
  #ifdef BUILD_NO_CALL
    .invertFunc   = stdInvertFunction,
  #endif
    .combineFunc  = stdCombine,
    .pPartialFunc = "_std_partial",
    .pStateFunc = "_std_state",
    .pMergeFunc   = "_stddev_merge"
  },
  {
    .name = "var_pop",
    .type = FUNCTION_TYPE_STDVAR,
    .classification = FUNC_MGT_AGG_FUNC | FUNC_MGT_TSMA_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .getEnvFunc   = getStdFuncEnv,
    .initFunc     = stdFunctionSetup,
    .processFunc  = stdFunction,
    .sprocessFunc = stdScalarFunction,
    .finalizeFunc = stdvarFinalize,
  #ifdef BUILD_NO_CALL
    .invertFunc   = stdInvertFunction,
  #endif
    .combineFunc  = stdCombine,
    .pPartialFunc = "_std_partial",
    .pStateFunc = "_std_state",
    .pMergeFunc   = "_stdvar_merge"
  },
  {
    .name = "_stdvar_merge",
    .type = FUNCTION_TYPE_STDVAR_MERGE,
    .classification = FUNC_MGT_AGG_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateStdMerge,
    .getEnvFunc   = getStdFuncEnv,
    .initFunc     = stdFunctionSetup,
    .processFunc  = stdFunctionMerge,
    .finalizeFunc = stdvarFinalize,
  #ifdef BUILD_NO_CALL
    .invertFunc   = stdInvertFunction,
  #endif
    .combineFunc  = stdCombine,
    .pPartialFunc = "_std_state_merge",
    .pMergeFunc = "_stdvar_merge",
  },
  {
    .name = "pi",
    .type = FUNCTION_TYPE_PI,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 0,
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translatePi,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = piFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "exp",
    .type = FUNCTION_TYPE_EXP,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = expFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "ln",
    .type = FUNCTION_TYPE_LN,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = lnFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "mod",
    .type = FUNCTION_TYPE_MOD,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateIn2NumOutDou,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = modFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "sign",
    .type = FUNCTION_TYPE_SIGN,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE}},
    .translateFunc = translateInOutNum,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = signFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "degrees",
    .type = FUNCTION_TYPE_DEGREES,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = degreesFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "radians",
    .type = FUNCTION_TYPE_RADIANS,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateInNumOutDou,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = radiansFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "truncate",
    .type = FUNCTION_TYPE_TRUNCATE,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateTrunc,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = truncFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "trunc",
    .type = FUNCTION_TYPE_TRUNCATE,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_NUMERIC_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateTrunc,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = truncFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "substring",
    .type = FUNCTION_TYPE_SUBSTR,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 3,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][2] = {.isLastParam = true,
                                           .startParam = 3,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE}},
    .translateFunc = translateSubstr,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = substrFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "substring_index",
    .type = FUNCTION_TYPE_SUBSTR_IDX,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 3,
                   .maxParamNum = 3,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = false,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][2] = {.isLastParam = true,
                                           .startParam = 3,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE}},
    .translateFunc = translateSubstrIdx,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = substrIdxFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "char",
    .type = FUNCTION_TYPE_CHAR,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = -1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = -1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_INTEGER_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE}},
    .translateFunc = translateChar,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = charFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "ascii",
    .type = FUNCTION_TYPE_ASCII,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateAscii,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = asciiFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "position",
    .type = FUNCTION_TYPE_POSITION,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translatePosition,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = positionFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "trim",
    .type = FUNCTION_TYPE_TRIM,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE}},
    .translateFunc = translateTrim,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = trimFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "replace",
    .type = FUNCTION_TYPE_REPLACE,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 3,
                   .maxParamNum = 3,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 3,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE}},
    .translateFunc = translateReplace,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = replaceFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "repeat",
    .type = FUNCTION_TYPE_REPEAT,
    .classification = FUNC_MGT_SCALAR_FUNC | FUNC_MGT_STRING_FUNC,
    .parameters = {.minParamNum = 2,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_VARCHAR_TYPE | FUNC_PARAM_SUPPORT_NCHAR_TYPE}},
    .translateFunc = translateRepeat,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = repeatFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "weekday",
    .type = FUNCTION_TYPE_WEEKDAY,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_UNIX_TS_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateWeekday,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = weekdayFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "dayofweek",
    .type = FUNCTION_TYPE_DAYOFWEEK,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_UNIX_TS_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateWeekday,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = dayofweekFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "week",
    .type = FUNCTION_TYPE_WEEK,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 2,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_UNIX_TS_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateWeek,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = weekFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "weekofyear",
    .type = FUNCTION_TYPE_WEEKOFYEAR,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 1,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = false,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_UNIX_TS_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .inputParaInfo[0][1] = {.isLastParam = true,
                                           .startParam = 2,
                                           .endParam = 2,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = true,
                                           .hasRange = false,
                                           .fixedValueSize = 8,
                                           .fixedNumValue = {0, 1, 2, 3, 4, 5, 6, 7}},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_BIGINT_TYPE}},
    .translateFunc = translateWeekofyear,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = weekofyearFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "rand",
    .type = FUNCTION_TYPE_RAND,
    .classification = FUNC_MGT_SCALAR_FUNC,
    .parameters = {.minParamNum = 0,
                   .maxParamNum = 1,
                   .paramInfoPattern = 1,
                   .inputParaInfo[0][0] = {.isLastParam = true,
                                           .startParam = 1,
                                           .endParam = 1,
                                           .validDataType = FUNC_PARAM_SUPPORT_INTEGER_TYPE | FUNC_PARAM_SUPPORT_NULL_TYPE,
                                           .validNodeType = FUNC_PARAM_SUPPORT_EXPR_NODE,
                                           .isPK = false,
                                           .isTs = false,
                                           .isFixedValue = false,
                                           .hasRange = false},
                   .outputParaInfo = {.validDataType = FUNC_PARAM_SUPPORT_DOUBLE_TYPE}},
    .translateFunc = translateRand,
    .getEnvFunc   = NULL,
    .initFunc     = NULL,
    .sprocessFunc = randFunction,
    .finalizeFunc = NULL
  },
  {
    .name = "forecast",
    .type = FUNCTION_TYPE_FORECAST,
    .classification = FUNC_MGT_TIMELINE_FUNC | FUNC_MGT_IMPLICIT_TS_FUNC |
                      FUNC_MGT_FORBID_STREAM_FUNC | FUNC_MGT_FORBID_SYSTABLE_FUNC | FUNC_MGT_KEEP_ORDER_FUNC | FUNC_MGT_PRIMARY_KEY_FUNC,    
    .translateFunc = translateForecast,
    .getEnvFunc    = getSelectivityFuncEnv,
    .initFunc      = functionSetup,
    .processFunc   = NULL,
    .finalizeFunc  = NULL,
    .estimateReturnRowsFunc = forecastEstReturnRows,
  },
    {
    .name = "_frowts",
    .type = FUNCTION_TYPE_FORECAST_ROWTS,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_FORECAST_PC_FUNC | FUNC_MGT_KEEP_ORDER_FUNC,
    .translateFunc = translateTimePseudoColumn,
    .getEnvFunc   = getTimePseudoFuncEnv,
    .initFunc     = NULL,
    .sprocessFunc = NULL,
    .finalizeFunc = NULL
  },
  {
    .name = "_flow",
    .type = FUNCTION_TYPE_FORECAST_LOW,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_FORECAST_PC_FUNC | FUNC_MGT_KEEP_ORDER_FUNC,
    .translateFunc = translateForecastConf,
    .getEnvFunc   = getForecastConfEnv,
    .initFunc     = NULL,
    .sprocessFunc = NULL,
    .finalizeFunc = NULL
  },
  {
    .name = "_fhigh",
    .type = FUNCTION_TYPE_FORECAST_HIGH,
    .classification = FUNC_MGT_PSEUDO_COLUMN_FUNC | FUNC_MGT_FORECAST_PC_FUNC | FUNC_MGT_KEEP_ORDER_FUNC,
    .translateFunc = translateForecastConf,
    .getEnvFunc   = getForecastConfEnv,
    .initFunc     = NULL,
    .sprocessFunc = NULL,
    .finalizeFunc = NULL
  },
};
// clang-format on

const int32_t funcMgtBuiltinsNum = (sizeof(funcMgtBuiltins) / sizeof(SBuiltinFuncDefinition));
