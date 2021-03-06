/*
 * Copyright (c) 2015, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "catchup.h"
#include "match.h"
#include "rose_sidecar_runtime.h"
#include "rose.h"
#include "util/fatbit.h"

static really_inline
void initContext(const struct RoseEngine *t, u8 *state, u64a offset,
                 struct hs_scratch *scratch, RoseCallback callback,
                 RoseCallbackSom som_callback, void *ctx) {
    struct RoseRuntimeState *rstate = getRuntimeState(state);
    struct RoseContext *tctxt = &scratch->tctxt;
    tctxt->t = t;
    tctxt->depth = rstate->stored_depth;
    tctxt->groups = loadGroups(t, state); /* TODO: diff groups for eod */
    tctxt->lit_offset_adjust = scratch->core_info.buf_offset
                             - scratch->core_info.hlen
                             + 1; // index after last byte
    tctxt->delayLastEndOffset = offset;
    tctxt->lastEndOffset = offset;
    tctxt->filledDelayedSlots = 0;
    tctxt->state = state;
    tctxt->cb = callback;
    tctxt->cb_som = som_callback;
    tctxt->userCtx = ctx;
    tctxt->lastMatchOffset = 0;
    tctxt->minMatchOffset = 0;
    tctxt->minNonMpvMatchOffset = 0;
    tctxt->next_mpv_offset = 0;
    tctxt->curr_anchored_loc = MMB_INVALID;
    tctxt->curr_row_offset = 0;

    scratch->catchup_pq.qm_size = 0;
    scratch->al_log_sum = 0; /* clear the anchored logs */

    fatbit_clear(scratch->aqa);
}

static rose_inline
hwlmcb_rv_t roseEodRunMatcher(const struct RoseEngine *t, u64a offset,
                              struct hs_scratch *scratch,
                              const char is_streaming) {
    assert(t->ematcherOffset);

    size_t eod_len;
    const u8 *eod_data;
    if (!is_streaming) { /* Block */
        eod_data = scratch->core_info.buf;
        eod_len = scratch->core_info.len;
    } else { /* Streaming */
        eod_len = scratch->core_info.hlen;
        eod_data = scratch->core_info.hbuf;
    }

    assert(eod_data);
    assert(eod_len);

    // If we don't have enough bytes to produce a match from an EOD table scan,
    // there's no point scanning.
    if (eod_len < t->eodmatcherMinWidth) {
        DEBUG_PRINTF("len=%zu < eodmatcherMinWidth=%u\n", eod_len,
                     t->eodmatcherMinWidth);
        return MO_CONTINUE_MATCHING;
    }

    // Ensure that we only need scan the last N bytes, where N is the length of
    // the eod-anchored matcher region.
    size_t adj = eod_len - MIN(eod_len, t->ematcherRegionSize);

    DEBUG_PRINTF("eod offset=%llu, eod length=%zu\n", offset, eod_len);

    struct RoseContext *tctxt = &scratch->tctxt;

    /* update side_curr for eod_len */
    tctxt->side_curr = offset - eod_len;

    /* no need to enable any sidecar groups as they are for .*A.* constructs
     * not allowed in the eod table */

    const struct HWLM *etable = getELiteralMatcher(t);

    hwlmExec(etable, eod_data, eod_len, adj, roseCallback, tctxt, tctxt->groups);

    // We may need to fire delayed matches
    u8 dummy_delay_mask = 0;
    return cleanUpDelayed(0, offset, tctxt, &dummy_delay_mask);
}

static rose_inline
int roseEodRunIterator(const struct RoseEngine *t, u8 *state, u64a offset,
                       struct hs_scratch *scratch) {
    if (!t->eodIterOffset) {
        return MO_CONTINUE_MATCHING;
    }

    const struct RoseRole *roleTable = getRoleTable(t);
    const struct RosePred *predTable = getPredTable(t);
    const struct RoseIterMapping *iterMapBase
        = getByOffset(t, t->eodIterMapOffset);
    const struct mmbit_sparse_iter *it = getByOffset(t, t->eodIterOffset);
    assert(ISALIGNED(iterMapBase));
    assert(ISALIGNED(it));

    // Sparse iterator state was allocated earlier
    struct mmbit_sparse_state *s = scratch->sparse_iter_state;
    struct fatbit *handled_roles = scratch->handled_roles;

    const u32 numStates = t->rolesWithStateCount;

    void *role_state = getRoleState(state);
    u32 idx = 0;
    u32 i = mmbit_sparse_iter_begin(role_state, numStates, &idx, it, s);

    fatbit_clear(handled_roles);

    for (; i != MMB_INVALID;
           i = mmbit_sparse_iter_next(role_state, numStates, i, &idx, it, s)) {
        DEBUG_PRINTF("pred state %u (iter idx=%u) is on\n", i, idx);
        const struct RoseIterMapping *iterMap = iterMapBase + idx;
        const struct RoseIterRole *roles = getByOffset(t, iterMap->offset);
        assert(ISALIGNED(roles));

        DEBUG_PRINTF("%u roles to consider\n", iterMap->count);
        for (u32 j = 0; j != iterMap->count; j++) {
            u32 role = roles[j].role;
            assert(role < t->roleCount);
            DEBUG_PRINTF("checking role %u, pred %u:\n", role, roles[j].pred);
            const struct RoseRole *tr = roleTable + role;

            if (fatbit_isset(handled_roles, t->roleCount, role)) {
                DEBUG_PRINTF("role %u already handled by the walk, skip\n",
                             role);
                continue;
            }

            // Special case: if this role is a trivial case (pred type simple)
            // we don't need to check any history and we already know the pred
            // role is on.
            if (tr->flags & ROSE_ROLE_PRED_SIMPLE) {
                DEBUG_PRINTF("pred type is simple, no need for checks\n");
            } else {
                assert(roles[j].pred < t->predCount);
                const struct RosePred *tp = predTable + roles[j].pred;
                if (!roseCheckPredHistory(tp, offset)) {
                    continue;
                }
            }

            /* mark role as handled so we don't touch it again in this walk */
            fatbit_set(handled_roles, t->roleCount, role);

            DEBUG_PRINTF("fire report for role %u, report=%u\n", role,
                         tr->reportId);
            int rv = scratch->tctxt.cb(offset, tr->reportId,
                                       scratch->tctxt.userCtx);
            if (rv == MO_HALT_MATCHING) {
                return MO_HALT_MATCHING;
            }
        }
    }

    return MO_CONTINUE_MATCHING;
}

static rose_inline
void roseCheckNfaEod(const struct RoseEngine *t, u8 *state,
                     struct hs_scratch *scratch, u64a offset,
                     const char is_streaming) {
    /* data, len is used for state decompress, should be full available data */
    const u8 *aa = getActiveLeafArray(t, state);
    const u32 aaCount = t->activeArrayCount;

    u8 key = 0;

    if (is_streaming) {
        const u8 *eod_data = scratch->core_info.hbuf;
        size_t eod_len = scratch->core_info.hlen;
        key = eod_len ? eod_data[eod_len - 1] : 0;
    }

    for (u32 qi = mmbit_iterate(aa, aaCount, MMB_INVALID); qi != MMB_INVALID;
         qi = mmbit_iterate(aa, aaCount, qi)) {
        const struct NfaInfo *info = getNfaInfoByQueue(t, qi);
        const struct NFA *nfa = getNfaByInfo(t, info);

        if (!nfaAcceptsEod(nfa)) {
            DEBUG_PRINTF("nfa %u does not accept eod\n", qi);
            continue;
        }

        DEBUG_PRINTF("checking nfa %u\n", qi);

        char *fstate = scratch->fullState + info->fullStateOffset;
        const char *sstate = (const char *)state + info->stateOffset;

        if (is_streaming) {
            // Decompress stream state.
            nfaExpandState(nfa, fstate, sstate, offset, key);
        }

        nfaCheckFinalState(nfa, fstate, sstate, offset, scratch->tctxt.cb,
                           scratch->tctxt.cb_som, scratch->tctxt.userCtx);
    }
}

static rose_inline
void cleanupAfterEodMatcher(const struct RoseEngine *t, u8 *state, u64a offset,
                            struct hs_scratch *scratch) {
    struct RoseContext *tctxt = &scratch->tctxt;

    // Flush history to make sure it's consistent.
    roseFlushLastByteHistory(t, state, offset, tctxt);

    // Catch up the sidecar to cope with matches raised in the etable.
    catchup_sidecar(tctxt, offset);
}

static rose_inline
void roseCheckEodSuffixes(const struct RoseEngine *t, u8 *state, u64a offset,
                          struct hs_scratch *scratch) {
    const u8 *aa = getActiveLeafArray(t, state);
    const u32 aaCount = t->activeArrayCount;
    UNUSED u32 qCount = t->queueCount;

    for (u32 qi = mmbit_iterate(aa, aaCount, MMB_INVALID); qi != MMB_INVALID;
         qi = mmbit_iterate(aa, aaCount, qi)) {
        const struct NfaInfo *info = getNfaInfoByQueue(t, qi);
        const struct NFA *nfa = getNfaByInfo(t, info);

        assert(nfaAcceptsEod(nfa));

        DEBUG_PRINTF("checking nfa %u\n", qi);

        assert(fatbit_isset(scratch->aqa, qCount, qi)); /* we have just been
                                                           triggered */

        char *fstate = scratch->fullState + info->fullStateOffset;
        const char *sstate = (const char *)state + info->stateOffset;

        struct mq *q = scratch->queues + qi;

        pushQueueNoMerge(q, MQE_END, scratch->core_info.len);

        q->context = NULL;
        /* rose exec is used as we don't want to / can't raise matches in the
         * history buffer. */
        char rv = nfaQueueExecRose(q->nfa, q, MO_INVALID_IDX);
        if (rv) { /* nfa is still alive */
            nfaCheckFinalState(nfa, fstate, sstate, offset, scratch->tctxt.cb,
                               scratch->tctxt.cb_som, scratch->tctxt.userCtx);
        }
    }
}

static really_inline
void roseEodExec_i(const struct RoseEngine *t, u8 *state, u64a offset,
                   struct hs_scratch *scratch, const char is_streaming) {
    assert(t);
    assert(scratch->core_info.buf || scratch->core_info.hbuf);
    assert(!scratch->core_info.buf || !scratch->core_info.hbuf);
    assert(!can_stop_matching(scratch));

    // Fire the special EOD event literal.
    if (t->hasEodEventLiteral) {
        DEBUG_PRINTF("firing eod event id %u at offset %llu\n",
                     t->eodLiteralId, offset);
        const struct core_info *ci = &scratch->core_info;
        size_t len = ci->buf ? ci->len : ci->hlen;
        assert(len || !ci->buf); /* len may be 0 if no history is required
                                  * (bounds checks only can lead to this) */

        roseRunEvent(len, t->eodLiteralId, &scratch->tctxt);
        if (can_stop_matching(scratch)) {
            DEBUG_PRINTF("user told us to stop\n");
            return;
        }
    }

    roseCheckNfaEod(t, state, scratch, offset, is_streaming);

    if (!t->eodIterOffset && !t->ematcherOffset) {
        DEBUG_PRINTF("no eod accepts\n");
        return;
    }

    // Handle pending EOD reports.
    int itrv = roseEodRunIterator(t, state, offset, scratch);
    if (itrv == MO_HALT_MATCHING) {
        return;
    }

    // Run the EOD anchored matcher if there is one.
    if (t->ematcherOffset) {
        assert(t->ematcherRegionSize);
        // Unset the reports we just fired so we don't fire them again below.
        mmbit_clear(getRoleState(state), t->rolesWithStateCount);
        mmbit_clear(getActiveLeafArray(t, state), t->activeArrayCount);
        sidecar_enabled_populate(t, scratch, state);

        hwlmcb_rv_t rv = roseEodRunMatcher(t, offset, scratch, is_streaming);
        if (rv == HWLM_TERMINATE_MATCHING) {
            return;
        }

        cleanupAfterEodMatcher(t, state, offset, scratch);

        // Fire any new EOD reports.
        roseEodRunIterator(t, state, offset, scratch);

        roseCheckEodSuffixes(t, state, offset, scratch);
    }
}

void roseEodExec(const struct RoseEngine *t, u8 *state, u64a offset,
                 struct hs_scratch *scratch, RoseCallback callback,
                 RoseCallbackSom som_callback, void *context) {
    assert(state);
    assert(scratch);
    assert(callback);
    assert(context);
    assert(t->requiresEodCheck);
    DEBUG_PRINTF("ci buf %p/%zu his %p/%zu\n", scratch->core_info.buf,
                 scratch->core_info.len, scratch->core_info.hbuf,
                 scratch->core_info.hlen);

    if (t->maxBiAnchoredWidth != ROSE_BOUND_INF
        && offset > t->maxBiAnchoredWidth) {
        DEBUG_PRINTF("bailing, we are beyond max width\n");
        /* also some of the history/state may be stale */
        return;
    }

    initContext(t, state, offset, scratch, callback, som_callback, context);

    roseEodExec_i(t, state, offset, scratch, 1);
}

static rose_inline
void prepForEod(const struct RoseEngine *t, u8 *state, size_t length,
                struct RoseContext *tctxt) {
    roseFlushLastByteHistory(t, state, length, tctxt);
    tctxt->lastEndOffset = length;
    if (t->requiresEodSideCatchup) {
        catchup_sidecar(tctxt, length);
    }
}

void roseBlockEodExec(const struct RoseEngine *t, u64a offset,
                      struct hs_scratch *scratch) {
    assert(t->requiresEodCheck);
    assert(t->maxBiAnchoredWidth == ROSE_BOUND_INF
           || offset <= t->maxBiAnchoredWidth);

    assert(!can_stop_matching(scratch));

    u8 *state = (u8 *)scratch->core_info.state;

    // Ensure that history is correct before we look for EOD matches
    prepForEod(t, state, scratch->core_info.len, &scratch->tctxt);

    roseEodExec_i(t, state, offset, scratch, 0);
}
