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

#ifndef _TD_PLANNER_H_
#define _TD_PLANNER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "taosmsg.h"

#define QUERY_TYPE_MERGE       1
#define QUERY_TYPE_PARTIAL     2
#define QUERY_TYPE_SCAN        3

enum OPERATOR_TYPE_E {
  OP_Unknown,
#define INCLUDE_AS_ENUM
#include "plannerOp.h"
#undef INCLUDE_AS_ENUM
  OP_TotalNum
};

struct SEpSet;
struct SQueryStmtInfo;

typedef SSchema SSlotSchema;

typedef struct SDataBlockSchema {
  SSlotSchema        *pSchema;
  int32_t             numOfCols;    // number of columns
} SDataBlockSchema;

typedef struct SQueryNodeBasicInfo {
  int32_t     type;          // operator type
  const char *name;          // operator name
} SQueryNodeBasicInfo;

typedef struct SPhyNode {
  SQueryNodeBasicInfo info;
  SArray             *pTargets;      // target list to be computed or scanned at this node
  SArray             *pConditions;   // implicitly-ANDed qual conditions
  SDataBlockSchema    targetSchema;
  // children plan to generated result for current node to process
  // in case of join, multiple plan nodes exist.
  SArray             *pChildren;
  struct SPhyNode    *pParent;
} SPhyNode;

typedef struct SScanPhyNode {
  SPhyNode    node;
  uint64_t    uid;  // unique id of the table
  int8_t      tableType;
} SScanPhyNode;

typedef SScanPhyNode SSystemTableScanPhyNode;
typedef SScanPhyNode STagScanPhyNode;

typedef struct STableScanPhyNode {
  SScanPhyNode scan;
  uint8_t      scanFlag;         // denotes reversed scan of data or not
  STimeWindow  window;
  SArray      *pTagsConditions; // implicitly-ANDed tag qual conditions
} STableScanPhyNode;

typedef STableScanPhyNode STableSeqScanPhyNode;

typedef struct SProjectPhyNode {
  SPhyNode node;
} SProjectPhyNode;

typedef struct SExchangePhyNode {
  SPhyNode    node;
  uint64_t    templateId;
  SArray     *pSourceEpSet;  // SEpSet
} SExchangePhyNode;

typedef struct SSubplanId {
  uint64_t queryId;
  uint64_t templateId;
  uint64_t subplanId;
} SSubplanId;

typedef struct SSubplan {
  SSubplanId id;          // unique id of the subplan
  int32_t   type;         // QUERY_TYPE_MERGE|QUERY_TYPE_PARTIAL|QUERY_TYPE_SCAN
  int32_t   level;        // the execution level of current subplan, starting from 0.
  SEpSet    execEpSet;    // for the scan sub plan, the optional execution node
  SArray   *pChildern;    // the datasource subplan,from which to fetch the result
  SArray   *pParents;     // the data destination subplan, get data from current subplan
  SPhyNode *pNode;        // physical plan of current subplan
} SSubplan;

typedef struct SQueryDag {
  SArray  *pSubplans; // Element is SArray*, and nested element is SSubplan. The execution level of subplan, starting from 0.
} SQueryDag;

/**
 * Create the physical plan for the query, according to the AST.
 */
int32_t qCreateQueryDag(const struct SQueryStmtInfo* pQueryInfo, struct SEpSet* pQnode, struct SQueryDag** pDag);

int32_t qExplainQuery(const struct SQueryStmtInfo* pQueryInfo, struct SEpSet* pQnode, char** str);


/**
 * Convert to subplan to string for the scheduler to send to the executor
 */
int32_t qSubPlanToString(struct SSubplan *pPhyNode, char** str);

/**
 * Destroy the physical plan.
 * @param pQueryPhyNode
 * @return
 */
void* qDestroyQueryDag(struct SQueryDag* pDag);

#ifdef __cplusplus
}
#endif

#endif /*_TD_PLANNER_H_*/