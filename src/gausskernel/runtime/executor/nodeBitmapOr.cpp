/* -------------------------------------------------------------------------
 *
 * nodeBitmapOr.cpp
 *	  routines to handle BitmapOr nodes.
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/runtime/executor/nodeBitmapOr.cpp
 *
 * -------------------------------------------------------------------------
 *
 * INTERFACE ROUTINES
 *		ExecInitBitmapOr	- initialize the BitmapOr node
 *		MultiExecBitmapOr	- retrieve the result bitmap from the node
 *		ExecEndBitmapOr		- shut down the BitmapOr node
 *		ExecReScanBitmapOr	- rescan the BitmapOr node
 *
 *	 NOTES
 *		BitmapOr nodes don't make use of their left and right
 *		subtrees, rather they maintain a list of subplans,
 *		much like Append nodes.  The logic is much simpler than
 *		Append, however, since we needn't cope with forward/backward
 *		execution.
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "executor/exec/execdebug.h"
#include "executor/node/nodeBitmapOr.h"
#include "miscadmin.h"

/* ----------------------------------------------------------------
 *		ExecInitBitmapOr
 *
 *		Begin all of the subscans of the BitmapOr node.
 * ----------------------------------------------------------------
 */
BitmapOrState* ExecInitBitmapOr(BitmapOr* node, EState* estate, int eflags)
{
    BitmapOrState* bitmaporstate = makeNode(BitmapOrState);
    PlanState** bitmapplanstates;
    int nplans;
    int i;
    ListCell* l = NULL;
    Plan* initNode = NULL;

    /* check for unsupported flags */
    Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

    /*
     * Set up empty vector of subplan states
     */
    nplans = list_length(node->bitmapplans);

    bitmapplanstates = (PlanState**)palloc0(nplans * sizeof(PlanState*));

    /*
     * create new BitmapOrState for our BitmapOr node
     */
    bitmaporstate->ps.plan = (Plan*)node;
    bitmaporstate->ps.state = estate;
    bitmaporstate->bitmapplans = bitmapplanstates;
    bitmaporstate->nplans = nplans;

    /*
     * Miscellaneous initialization
     *
     * BitmapOr plans don't have expression contexts because they never call
     * ExecQual or ExecProject.  They don't need any tuple slots either.
     */
    /*
     * call ExecInitNode on each of the plans to be executed and save the
     * results into the array "bitmapplanstates".
     */
    i = 0;
    foreach (l, node->bitmapplans) {
        initNode = (Plan*)lfirst(l);
        bitmapplanstates[i] = ExecInitNode(initNode, estate, eflags);
        i++;
    }

    return bitmaporstate;
}

/* ----------------------------------------------------------------
 *	   MultiExecBitmapOr
 * ----------------------------------------------------------------
 */
Node* MultiExecBitmapOr(BitmapOrState* node)
{
    PlanState** bitmapplans;
    int nplans;
    int i;
    TIDBitmap* result = NULL;
    bool isUstore = ((BitmapOr*)node->ps.plan)->is_ustore;

    /* must provide our own instrumentation support */
    if (node->ps.instrument) {
        InstrStartNode(node->ps.instrument);
    }

    /*
     * get information from the node
     */
    bitmapplans = node->bitmapplans;
    nplans = node->nplans;

    /*
     * Scan all the subplans and OR their result bitmaps
     */
    for (i = 0; i < nplans; i++) {
        PlanState* subnode = bitmapplans[i];
        subnode->hbktScanSlot.currSlot = node->ps.hbktScanSlot.currSlot;
        TIDBitmap* subresult = NULL;

        /*
         * We can special-case BitmapIndexScan children to avoid an explicit
         * tbm_union step for each child: just pass down the current result
         * bitmap and let the child OR directly into it.
         */
        if (IsA(subnode, BitmapIndexScanState)) {
            /* first subplan */
            if (result == NULL) {
                /* XXX should we use less than u_sess->attr.attr_memory.work_mem for this? */
                long maxbytes = u_sess->attr.attr_memory.work_mem * 1024L;
                result = tbm_create(maxbytes,
                                    RelationIsGlobalIndex(((BitmapIndexScanState *)subnode)->biss_RelationDesc),
                                    RelationIsCrossBucketIndex(((BitmapIndexScanState *)subnode)->biss_RelationDesc),
                                    RelationIsPartitioned(((BitmapIndexScanState *)subnode)->biss_RelationDesc),
                                    isUstore);
            }

            ((BitmapIndexScanState*)subnode)->biss_result = result;

            subresult = (TIDBitmap*)MultiExecProcNode(subnode);
            if (subresult != result) {
                ereport(ERROR,
                    (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                        errmsg("unrecognized result from BitmapIndexScan subplan when execute BitmapOr")));
            }
        } else {
            /* standard implementation */
            subresult = (TIDBitmap*)MultiExecProcNode(subnode);
            if (subresult == NULL || !IsA(subresult, TIDBitmap)) {
                ereport(ERROR,
                    (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                        errmsg("unrecognized result from non-BitmapIndexScan subplan when execute BitmapOr")));
            }

            if (result == NULL) {
                result = subresult; /* first subplan */
            } else {
                TBMHandler tbm_handler = tbm_get_handler(result);
                if (tbm_is_global(result) != tbm_is_global(subresult)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_UNRECOGNIZED_NODE_TYPE),
                            errmsg(
                                "do not support bitmap index scan for global index and local index simultaneously.")));
                }
                tbm_handler._union(result, subresult);
                tbm_free(subresult);
            }
        }
    }

    /* We could return an empty result set here? */
    if (result == NULL) {
        ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("BitmapOr doesn't support zero inputs")));
    }

    /* must provide our own instrumentation support */
    if (node->ps.instrument) {
        InstrStopNode(node->ps.instrument, 0 /* XXX */);
    }

    return (Node*)result;
}

/* ----------------------------------------------------------------
 *		ExecEndBitmapOr
 *
 *		Shuts down the subscans of the BitmapOr node.
 *
 *		Returns nothing of interest.
 * ----------------------------------------------------------------
 */
void ExecEndBitmapOr(BitmapOrState* node)
{
    PlanState** bitmapplans;
    int nplans;
    int i;

    /*
     * get information from the node
     */
    bitmapplans = node->bitmapplans;
    nplans = node->nplans;

    /*
     * shut down each of the subscans (that we've initialized)
     */
    for (i = 0; i < nplans; i++) {
        if (bitmapplans[i])
            ExecEndNode(bitmapplans[i]);
    }
}

void ExecReScanBitmapOr(BitmapOrState* node)
{
    int i;

    for (i = 0; i < node->nplans; i++) {
        PlanState* subnode = node->bitmapplans[i];

        /*
         * ExecReScan doesn't know about my subplans, so I have to do
         * changed-parameter signaling myself.
         */
        if (node->ps.chgParam != NULL)
            UpdateChangedParamSet(subnode, node->ps.chgParam);

        /*
         * If chgParam of subnode is not null then plan will be re-scanned by
         * first ExecProcNode.
         */
        if (subnode->chgParam == NULL)
            ExecReScan(subnode);
    }
}
