/****************************************************************************
 *
 * Copyright (C) 2014-2015 Cisco and/or its affiliates. All rights reserved.
 * Copyright (C) 2009-2013 Sourcefire, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 2 as
 * published by the Free Software Foundation.  You may not use, modify or
 * distribute this program under any other version of the GNU General
 * Public License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 ****************************************************************************/

/* @file  sfrf.c
 * @brief rate filter implementation for Snort
 * @ingroup rate_filter
 * @author Dilbagh Chahal
 */
/* @ingroup rate_filter
 * @{
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef WIN32
#include <netinet/in.h>
#include <arpa/inet.h>

#endif /* !WIN32 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "snort.h"
#include "parser/IpAddrSet.h"
#include "generators.h"
#include "rules.h"
#include "treenodes.h"
#include "sfrf.h"
#include "util.h"
#include "sfPolicyData.h"
#include "sfPolicyUserData.h"

// Number of hash rows for gid 1 (rules)
#define SFRF_GEN_ID_1_ROWS 4096
// Number of hash rows for non-zero gid
#define SFRF_GEN_ID_ROWS   512

// maximum number of norevert rate_filter configuration allowed.
#define SFRF_NO_REVERT_LIMIT 1000

// private data ...
/* Key to find tracking nodes in trackingHash.
 */
typedef struct
{
    ///policy identifier.
    tSfPolicyId policyId;

    /* Internally generated threshold identity for a configured threshold.
    */
    int tid;

    /* Stores either source or destination IP address on a matching packet, depending on
     * whether dos threshold is tracking by source or destination IP address. For tracking
     * by rule, it is cleared out (all 0s).
     */
    snort_ip ip;

} tSFRFTrackingNodeKey ;

/* Tracking node for rate_filter. One node is created on fly, in tracking
 * hash for each threshold configure (identified by Tid) and source or
 * destination IP address.  For rule based tracking, IP is cleared in the
 * created node. Nodes are deleted when hash performs ANR on hash.
 */
typedef struct
{
    // automatically initialized to FS_NEW when allocated
    FilterState filterState;

#ifdef SFRF_OVER_RATE
    int overRate;    // 0 = count not exceeded in prior seconds
    time_t tlast;    // time of most recent event
#endif

    /* number of packets counted against a specific IP address or threshold.
     */
    unsigned count;

    /* time when this sampling period started.
     */
    time_t tstart;

    /*  time when new action was activated due to rate limit exceeding.
    */
    time_t revertTime;

} tSFRFTrackingNode;

SFXHASH *rf_hash = NULL;

// private methods ...
static int _checkThreshold(
    tSFRFConfigNode*,
    tSFRFTrackingNode*,
    time_t curTime
);

static int _checkSamplingPeriod(
    tSFRFConfigNode*,
    tSFRFTrackingNode*,
    time_t curTime
);

static tSFRFTrackingNode *_getSFRFTrackingNode(
    snort_ip_p,
    unsigned tid,
    time_t curTime
);

static void _updateDependentThresholds(
    RateFilterConfig *config,
    unsigned gid,
    unsigned sid,
    snort_ip_p sip,
    snort_ip_p dip,
    time_t curTime
);

// public methods ...
/* Create a new threshold global context
 *
 * Create a threshold table, initialize the threshold system, and optionally
 * limit it's memory usage.
 *
 * @param nbytes maximum memory to use for thresholding objects, in bytes.
 * @return  pointer to newly created tSFRFContext
*/
#define SFRF_BYTES (sizeof(tSFRFTrackingNodeKey) + sizeof(tSFRFTrackingNode))

static void SFRF_New( unsigned nbytes )
{
    int nrows;

    /* Calc max ip nodes for this memory */
    if ( nbytes < SFRF_BYTES )
    {
        nbytes = SFRF_BYTES;
    }
    nrows = nbytes / (SFRF_BYTES);

    /* Create global hash table for all of the IP Nodes */
    rf_hash = sfxhash_new(
        nrows,  /* try one node per row - for speed */
        sizeof(tSFRFTrackingNodeKey), /* keys size */
        sizeof(tSFRFTrackingNode),     /* data size */
        nbytes,                  /* memcap **/
        1,         /* ANR flag - true ?- Automatic Node Recovery=ANR */
        0,         /* ANR callback - none */
        0,         /* user freemem callback - none */
        1) ;      /* Recycle nodes ?*/
}

void SFRF_Delete (void)
{
    if ( !rf_hash )
        return;

    sfxhash_delete(rf_hash);
    rf_hash = NULL;
}

void SFRF_Flush (void)
{
    if ( rf_hash )
        sfxhash_make_empty(rf_hash);
}

static void SFRF_ConfigNodeFree(void *item)
{
    tSFRFConfigNode *node = (tSFRFConfigNode *)item;

    if (node == NULL)
        return;

    if (node->applyTo != NULL)
    {
        IpAddrSetDestroy(node->applyTo);
    }

    free(node);
}

/* free tSFRFSidNode and related buffers.
 *
 * @param item - pointer to tSFRFSidNode to be freed.
 * @returns void
 */
static void SFRF_SidNodeFree(void* item)
{
    tSFRFSidNode* pSidnode = (tSFRFSidNode*)item;
    sflist_free_all(pSidnode->configNodeList, SFRF_ConfigNodeFree);
    free(pSidnode);
}

/*  Add a permanent threshold object to the threshold table. Multiple
 * objects may be defined for each gid and sid pair. Internally
 * a unique threshold id is generated for each pair.
 *
 * Threshold objects track the number of events seen during the time
 * interval specified by seconds. Depending on the type of threshold
 * object and the count value, the thresholding object determines if
 * the current event should be logged or dropped.
 *
 * @param pContext Threshold object from SFRF_ContextNew()
 * @param cfgNode Permanent Thresholding Object
 *
 * @return @retval  0 successfully added the thresholding object, !0 otherwise
*/
int SFRF_ConfigAdd(SnortConfig *sc, RateFilterConfig *rf_config, tSFRFConfigNode *cfgNode)
{
    SFGHASH* genHash;
    int nrows;
    int hstatus;
    tSFRFSidNode* pSidNode;
    tSFRFConfigNode* pNewConfigNode;
    tSFRFGenHashKey key = {0,0};
    tSfPolicyId policy_id = getParserPolicy(sc);

    // Auto init - memcap must be set 1st, which is not really a problem
    if ( rf_hash == NULL )
    {
        SFRF_New(rf_config->memcap);

        if ( rf_hash == NULL )
            return -1;
    }

    if ((rf_config == NULL) || (cfgNode == NULL))
        return -1;

    if ( (cfgNode->sid == 0 ) || (cfgNode->gid == 0) )
        return -1;

    if ( cfgNode->gid >= SFRF_MAX_GENID )
        return -1;

    if ( cfgNode->count < 1 )
        return -1;

    if ( cfgNode->timeout == 0 )
    {
        if ( rf_config->noRevertCount >= SFRF_NO_REVERT_LIMIT )
            return -1;

        rf_config->noRevertCount++;
    }

    /* Check for an existing 'gid' entry, if none found then create one. */
    /* Get the hash table for this gid */
    genHash = rf_config->genHash[cfgNode->gid];

    if ( !genHash )
    {
        if ( cfgNode->gid == 1 )/* patmatch rules gid, many rules */
        {
            nrows= SFRF_GEN_ID_1_ROWS;
        }
        else  /* other gid's */
        {
            nrows= SFRF_GEN_ID_ROWS;
        }

        /* Create the hash table for this gid */
        genHash = sfghash_new( nrows, sizeof(tSFRFGenHashKey), 0, SFRF_SidNodeFree );
        if ( !genHash )
            return -2;

        rf_config->genHash[cfgNode->gid] = genHash;
    }

    key.sid = cfgNode->sid;
    key.policyId = policy_id;

    /* Check if sid is already in the table - if not allocate and add it */
    pSidNode = (tSFRFSidNode*)sfghash_find( genHash, (void*)&key );
    if ( !pSidNode )
    {
        /* Create the pSidNode hash node data */
        pSidNode = (tSFRFSidNode*)calloc(1,sizeof(tSFRFSidNode));
        if ( !pSidNode )
            return -3;

        pSidNode->gid = cfgNode->gid;
        pSidNode->sid = cfgNode->sid;
        pSidNode->configNodeList = sflist_new();

        if ( !pSidNode->configNodeList )
        {
            free(pSidNode);
            return -4;
        }

        /* Add the pSidNode to the hash table */
        hstatus = sfghash_add( genHash, (void*)&key, pSidNode );
        if ( hstatus )
        {
            sflist_free(pSidNode->configNodeList);
            free(pSidNode);
            return -5;
        }
    }

    /* Create a tSFRFConfigNode for this tSFRFSidNode (Object) */
    pNewConfigNode = (tSFRFConfigNode*)calloc(1,sizeof(tSFRFConfigNode));
    if ( !pNewConfigNode )
    {
        sflist_free(pSidNode->configNodeList);
        free(pSidNode);
        return -6;
    }

    *pNewConfigNode = *cfgNode;

    rf_config->count++;

    /* Copy the node parameters, with unique internally assigned tid */
    pNewConfigNode->tid = rf_config->count;
    if ( pNewConfigNode->tid == 0 )
    {
        // tid overflow. rare but possible
        free(pNewConfigNode);
        sflist_free(pSidNode->configNodeList);
        free(pSidNode);
        return -6;
    }


#ifdef SFRF_DEBUG
    printf("--%d-%d-%d: Threshold node added to tail of list\n",
            pNewConfigNode->tid,
            pNewConfigNode->gid,
            pNewConfigNode->sid);
    fflush(stdout);
#endif
    sflist_add_tail(pSidNode->configNodeList,pNewConfigNode);

    return 0;
}


#ifdef SFRF_DEBUG
static char* get_netip(snort_ip_p ip)
{
    return sfip_ntoa(ip);
}

#endif // SFRF_DEBUG

/*
 *
 *  Find/Test/Add an event against a single threshold object.
 *  Events without thresholding objects are automatically loggable.
 *
 *  @param pContext     Threshold table pointer
 *  @param cfgNode Permanent Thresholding Object
 *  @param ip     Event/Packet Src IP address- should be host ordered for comparison
 *  @param curTime Current Event/Packet time in seconds
 *  @param op operation of type SFRF_COUNT_OPERATION
 *
 *  @return  integer
 *  @retval   !0 : rate limit is reached. Return value contains new action.
 *  @retval   0 : Otherwise
 */
static int SFRF_TestObject(
    tSFRFConfigNode* cfgNode,
    snort_ip_p ip,
    time_t curTime,
    SFRF_COUNT_OPERATION op
) {
    tSFRFTrackingNode* dynNode;
    int retValue = -1;

    dynNode = _getSFRFTrackingNode(ip, cfgNode->tid, curTime);

    if ( dynNode == NULL )
        return retValue;

    if ( _checkSamplingPeriod(cfgNode, dynNode, curTime) != 0 )
    {
#ifdef SFRF_DEBUG
        printf("...Sampling period reset\n");
        fflush(stdout);
#endif
    }

    switch (op)
    {
        case SFRF_COUNT_INCREMENT:
            if ( (dynNode->count+1) != 0 )
            {
                dynNode->count++;
            }
            break;
        case SFRF_COUNT_DECREMENT:
            if ( cfgNode->seconds == 0 )
            {
                // count can be decremented only for total count, and not for rate
                if ( dynNode->count != 0 )
                {
                    dynNode->count--;
                }
            }
            break;
        case SFRF_COUNT_RESET:
            dynNode->count = 0;
            break;
        default:
            break;
    }

    retValue = _checkThreshold(cfgNode, dynNode, curTime);

    // we drop after the session count has been incremented
    // but the decrement will never come so we "fix" it here
    // if the count were not incremented in such cases, the
    // threshold would never be exceeded.
    if ( !cfgNode->seconds && dynNode->count > cfgNode->count )
        if ( cfgNode->newAction == RULE_TYPE__DROP )
            dynNode->count--;

#ifdef SFRF_DEBUG
    printf("--SFRF_DEBUG: %d-%d-%d: %d Packet IP %s, op: %d, count %d, action %d\n",
            cfgNode->tid, cfgNode->gid,
            cfgNode->sid, (unsigned) curTime, get_netip(ip), op,
            dynNode->count, retValue);
    fflush(stdout);
#endif
    return retValue;
}

static inline int SFRF_AppliesTo(tSFRFConfigNode* pCfg, snort_ip_p ip)
{
    return ( !pCfg->applyTo || IpAddrSetContains(pCfg->applyTo, ip) );
}

/* Test a an event against the threshold database. Events without thresholding
 * objects are automatically loggable.
 *
 * @param pContext     Threshold table pointer
 * @param gid  Generator Id from the event
 * @param sid  Signature Id from the event
 * @param sip     Event/Packet Src IP address
 * @param dip     Event/Packet Dst IP address
 * @param curTime Current Event/Packet time
 * @param op operation of type SFRF_COUNT_OPERATION
 *
 * @return  -1 if packet is within dos_threshold and therefore action is allowed.
 *         >=0 if packet violates a dos_threshold and therefore new_action should
 *             replace rule action. new_action value is returned.
 */
int SFRF_TestThreshold(
    RateFilterConfig *config,
    unsigned gid,
    unsigned sid,
    snort_ip_p sip,
    snort_ip_p dip,
    time_t curTime,
    SFRF_COUNT_OPERATION op
) {
    SFGHASH  *genHash;
    tSFRFSidNode* pSidNode;
    tSFRFConfigNode* cfgNode;
    int newStatus = -1;
    int status = -1;
    tSFRFGenHashKey key;
    tSfPolicyId policy_id = getIpsRuntimePolicy();

#ifdef SFRF_DEBUG
    printf("--%d-%d-%d: %s() entering\n", 0, gid, sid, __func__);
    fflush(stdout);
#endif

    if ( gid >= SFRF_MAX_GENID )
        return status; /* bogus gid */

    // Some events (like 'TCP connection closed' raised by preprocessor may
    // not have any configured threshold but may impact thresholds for other
    // events (like 'TCP connection opened'
    _updateDependentThresholds(config, gid, sid, sip, dip, curTime);

    /*
     *  Get the hash table for this gid
     */
    genHash = config->genHash [ gid ];
    if ( !genHash )
    {
#ifdef SFRF_DEBUG
        printf("--SFRF_DEBUG: %d-%d-%d: no hash table entry for gid\n", 0, gid, sid);
        fflush(stdout);
#endif
        return status;
    }

    /*
     * Check for any Permanent sid objects for this gid
     */
    key.sid = sid;
    key.policyId = policy_id;

    pSidNode = (tSFRFSidNode*)sfghash_find( genHash, (void*)&key );
    if ( !pSidNode )
    {
#ifdef SFRF_DEBUG
        printf("--SFRF_DEBUG: %d-%d-%d: no DOS THD object\n", 0, gid, sid);
        fflush(stdout);
#endif
        return status;
    }

    /* No List of Threshold objects - bail and log it */
    if ( !pSidNode->configNodeList )
    {
#ifdef SFRF_DEBUG
        printf("--SFRF_DEBUG: %d-%d-%d: No user configuration\n",
                0, gid, sid);
        fflush(stdout);
#endif
        return status;
    }

    /* For each permanent thresholding object, test/add/update the config object */
    /* We maintain a list of thd objects for each gid+sid */
    /* each object has it's own unique thd_id */
    for ( cfgNode  = (tSFRFConfigNode*)sflist_first(pSidNode->configNodeList);
          cfgNode != 0;
          cfgNode  = (tSFRFConfigNode*)sflist_next(pSidNode->configNodeList) )
    {
        switch (cfgNode->tracking)
        {
            case SFRF_TRACK_BY_SRC:
                if ( SFRF_AppliesTo(cfgNode, sip) )
                {
                    newStatus = SFRF_TestObject(cfgNode, sip, curTime, op);
                }
                break;

            case SFRF_TRACK_BY_DST:
                if ( SFRF_AppliesTo(cfgNode, dip) )
                {
                    newStatus = SFRF_TestObject(cfgNode, dip, curTime, op);
                }
                break;

            case SFRF_TRACK_BY_RULE:
                {
                    snort_ip cleared;
                    IP_CLEAR(cleared);
                    newStatus = SFRF_TestObject(cfgNode, IP_ARG(cleared), curTime, op);
                }
                break;

            default:
                // error case
                break;
        }

#ifdef SFRF_DEBUG
        printf("--SFRF_DEBUG: %d-%d-%d: Time %d, rate limit blocked: %d\n",
                cfgNode->tid, gid, sid, (unsigned)curTime, newStatus);
        fflush(stdout);
#endif

        // rate limit is reached
        if ( newStatus >= 0 && (status == -1) )
        {
            status = newStatus;
        }
    }

    // rate limit not reached
    return status;
}

/* A function to print the thresholding objects to stdout.
 *
 * @param pContext pointer to global threshold context
 * @return
 */
void SFRF_ShowObjects(RateFilterConfig *config)
{
    SFGHASH* genHash;
    tSFRFSidNode* pSidnode;
    tSFRFConfigNode* cfgNode;
    int gid;
    SFGHASH_NODE* sidHashNode;

    for ( gid=0;gid < SFRF_MAX_GENID ; gid++ )
    {
        genHash = config->genHash [ gid ];
        if ( !genHash )
        {
            continue;
        }

        printf("...GEN_ID = %u\n",gid);

        for ( sidHashNode  = sfghash_findfirst( genHash );
              sidHashNode != 0;
              sidHashNode  = sfghash_findnext( genHash ) )
        {
            /* Check for any Permanent sid objects for this gid */
            pSidnode = (tSFRFSidNode*)sidHashNode->data;

            printf(".....GEN_ID = %u, SIG_ID = %u, PolicyId = %u\n",gid, pSidnode->sid, pSidnode->policyId);

            /* For each permanent thresholding object, test/add/update the thd object */
            /* We maintain a list of thd objects for each gid+sid */
            /* each object has it's own unique thd_id */

            for ( cfgNode  = (tSFRFConfigNode*)sflist_first(pSidnode->configNodeList);
                  cfgNode != 0;
                  cfgNode = (tSFRFConfigNode*)sflist_next(pSidnode->configNodeList) )
            {
                printf(".........SFRF_ID  =%d\n",cfgNode->tid );
                printf(".........tracking =%d\n",cfgNode->tracking);
                printf(".........count    =%u\n",cfgNode->count);
                printf(".........seconds  =%u\n",cfgNode->seconds);
            }
        }
    }
}

/* Set sampling period rate limit
 *
 * @param cfgNode threshold configuration node
 * @param dynNode tracking node for a configured node
 * @param curTime for packet timestamp
 *
 * @returns 0 if continuing with old sampling period.
 *          1 if new sampling period is started.
 */
static int _checkSamplingPeriod(
    tSFRFConfigNode* cfgNode,
    tSFRFTrackingNode* dynNode,
    time_t curTime
) {
    unsigned dt;

    if ( cfgNode->seconds )
    {
        dt = (unsigned)(curTime - dynNode->tstart);
        if ( dt >= cfgNode->seconds )
        {   // observation period is over, start a new one
            dynNode->tstart = curTime;

#ifdef SFRF_OVER_RATE
            dt = (unsigned)(curTime - dynNode->tlast);
            if ( dt > cfgNode->seconds ) dynNode->overRate = 0;
            else dynNode->overRate = (dynNode->count > cfgNode->count);
            dynNode->tlast = curTime;
#endif
            dynNode->count = 0;
            return 1;
        }
    }
#ifdef SFRF_OVER_RATE
    else
    {
        dynNode->overRate = (dynNode->count > cfgNode->count);
    }
    dynNode->tlast = curTime;
#endif
    return 0;
}

/* Checks if rate limit is reached for a configured threshold.
 *
 * DOS Threshold monitoring is done is discrete time intervals specified by
 * 'cfgNode->seconds'. Once threshold action is activated, it stays active
 * for the revert timeout. Counters and seconds is maintained current at all
 * times. This may cause threshold action to be reactivated immediately if counter
 * is above threshold.
 * Threshold is tracked using a hash with ANR. This could cause some tracking nodes
 * to disappear when memory is low. Node deletion and subsequent creation will cause
 * rate limiting to start afresh for a specific stream.
 *
 * @param cfgNode threshold configuration node
 * @param dynNode tracking node for a configured node
 * @param curTime for packet timestamp
 *
 * @returns 0 if threshold is not reached
 *          1 otherwise
 */
static int _checkThreshold(
    tSFRFConfigNode* cfgNode,
    tSFRFTrackingNode* dynNode,
    time_t curTime
) {
    /* Once newAction is activated, it stays active for the revert timeout, unless ANR
     * causes the node itself to disappear.
     * Also note that we want to maintain the counters and rates update so that we reblock
     * offending traffic again quickly if it has not subsided.
     */
    if ( dynNode->filterState == FS_ON )
    {
        if ( (cfgNode->timeout != 0 )
            && ((unsigned)(curTime - dynNode->revertTime) >= cfgNode->timeout))
        {
#ifdef SFRF_OVER_RATE
            if ( dynNode->count > cfgNode->count || dynNode->overRate )
            {
#ifdef SFRF_DEBUG
                printf("...dos action continued, count %u\n", dynnode->count);
                fflush(stdout);
#endif
                dynNode->revertTime = curTime;
                return cfgNode->newAction;
            }
#endif
#ifdef SFRF_DEBUG
            printf("...dos action stopped, count %u\n", dynnode->count);
            fflush(stdout);
#endif
            dynNode->filterState = FS_OFF;
        }
        else
        {
#ifdef SFRF_DEBUG
            printf("...DOS action continued, count %u\n", dynNode->count);
            fflush(stdout);
#endif
            return cfgNode->newAction;
        }
    }

#ifdef SFRF_OVER_RATE
    if ( dynNode->count <= cfgNode->count && !dynNode->overRate )
#else
    if ( dynNode->count <= cfgNode->count )
#endif
    {
        // rate limit not reached.
#ifdef SFRF_DEBUG
        printf("...DOS action nop, count %u\n", dynNode->count);
        fflush(stdout);
#endif
        return -1;
    }

    // rate limit reached.
    dynNode->revertTime = curTime;
    dynNode->filterState = FS_ON;
#ifdef SFRF_OVER_RATE
    dynNode->overRate = 1;
#endif
#ifdef SFRF_DEBUG
    printf("...DOS action started, count %u\n", dynNode->count);
    fflush(stdout);
#endif

    return RULE_TYPE__MAX + cfgNode->newAction;
}


static void _updateDependentThresholds(
    RateFilterConfig *config,
    unsigned gid,
    unsigned sid,
    snort_ip_p sip,
    snort_ip_p dip,
    time_t curTime
) {
    if ( gid == GENERATOR_INTERNAL &&
         sid == INTERNAL_EVENT_SESSION_DEL )
    {
        // decrementing counters - this results in the following sequence:
        // 1. sfdos_thd_test_threshold(gid internal, sid DEL)
        // 2.    _updateDependentThresholds(gid internal, sid DEL)
        // 3.    |   sfdos_thd_test_threshold(gid internal, sid ADD)
        // 4.    |       _updateDependentThresholds(gid internal, sid ADD)
        // 5.    continue with regularly scheduled programming (ie step 1)

        SFRF_TestThreshold(config, gid, INTERNAL_EVENT_SESSION_ADD,
                sip, dip, curTime, SFRF_COUNT_DECREMENT);
        return;
    }
}

static tSFRFTrackingNode* _getSFRFTrackingNode(
    snort_ip_p ip,
    unsigned tid,
    time_t curTime
) {
    tSFRFTrackingNode* dynNode = NULL;
    tSFRFTrackingNodeKey key;
    SFXHASH_NODE * hnode = NULL;

    /* Setup key */
    key.ip = *(IP_PTR(ip));
    key.tid = tid;
    key.policyId = getNapRuntimePolicy();  // TBD-EDM should this be NAP or IPS?

    /*
     * Check for any Permanent sid objects for this gid or add this one ...
     */
    hnode = sfxhash_get_node(rf_hash, (void*)&key);
    if ( hnode && hnode->data )
    {
        dynNode = (tSFRFTrackingNode*)hnode->data;

        if ( dynNode->filterState == FS_NEW )
        {
            // first time initialization
            dynNode->tstart = curTime;
#ifdef SFRF_OVER_RATE
            dynNode->tlast = curTime;
#endif
            dynNode->filterState = FS_OFF;
        }
    }
    return dynNode;
}
/*@}*/

