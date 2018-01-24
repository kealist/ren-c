//
//  File: %d-eval.c
//  Summary: "Debug-Build Checks for the Evaluator"
//  Section: debug
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Rebol Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Due to the length of Do_Core() and how many debug checks it already has,
// some debug-only routines are separated out here.  (Note that these are in
// addition to the checks already done by Push_Frame() and Drop_Frame() time)
//
// * Do_Core_Expression_Checks_Debug() runs before each full "expression"
//   is evaluated, e.g. before each DO/NEXT step.  It makes sure the state
//   balanced completely--so no DS_PUSH that wasn't balanced by a DS_POP
//   or DS_DROP (for example).  It also trashes variables in the frame which
//   might accidentally carry over from one step to another, so that there
//   will be a crash instead of a casual reuse.
//
// * Do_Core_Exit_Checks_Debug() runs if the Do_Core() call makes it to the
//   end without a fail() longjmping out from under it.  It also checks to
//   make sure the state has balanced, and that the return result is
//   consistent with the state being returned.
//
// Because none of these routines are in the release build, they cannot have
// any side-effects that affect the interpreter's ordinary operation.
//

#include "sys-core.h"

#if !defined(NDEBUG)

//
//  Dump_Frame_Location: C
//
void Dump_Frame_Location(const RELVAL *current, REBFRM *f)
{
    if (FRM_IS_VALIST(f)) {
        //
        // NOTE: This reifies the va_list in the frame, and hence has side
        // effects.  It may need to be commented out if the problem you are
        // trapping with TICK_BREAKPOINT or C-DEBUG-BREAK was specifically
        // related to va_list frame processing.
        //
        const REBOOL truncated = TRUE;
        Reify_Va_To_Array_In_Frame(f, truncated);
    }

    DECLARE_LOCAL (dump);

    if (current != NULL) {
        Derelativize(dump, current, f->specifier);
        printf("Dump_Frame_Location() current\n");
        PROBE(dump);
    }

    if (f->value != NULL) {
        Derelativize(dump, f->value, f->specifier);
        printf("Dump_Frame_Location() next\n");
        PROBE(dump);
    }

    if (FRM_AT_END(f)) {
        printf("...then Dump_Frame_Location() is at end of array\n");
    }
    else {
        printf("Dump_Frame_Location() rest\n");

        Init_Any_Series_At_Core(
            dump,
            REB_BLOCK,
            SER(f->source.array),
            cast(REBCNT, f->source.index),
            f->specifier
        );
        PROBE(dump);
    }
}


// These are checks common to Expression and Exit checks (hence also common
// to the "end of Start" checks, since that runs on the first expression)
//
static void Do_Core_Shared_Checks_Debug(REBFRM *f) {
    //
    // There shouldn't have been any "accumulated state", in the sense that
    // we should be back where we started in terms of the data stack, the
    // mold buffer position, the outstanding manual series allocations, etc.
    //
    // Because this check is a bit expensive it is lightened up and used in
    // the exit case only.  But re-enable it to help narrowing down an
    // imbalanced state discovered on an exit.
    //
#ifdef BALANCE_CHECK_EVERY_EVALUATION_STEP
    ASSERT_STATE_BALANCED(&f->state);
#endif

    assert(f == FS_TOP);
    assert(f->state.top_chunk == TG_Top_Chunk);
    assert(DSP == f->dsp_orig);

    if (f->source.array != NULL) {
        assert(NOT(IS_POINTER_TRASH_DEBUG(f->source.array)));
        assert(
            f->source.index != TRASHED_INDEX
            && f->source.index != END_FLAG
            && f->source.index != THROWN_FLAG
            && f->source.index != VA_LIST_FLAG
        ); // END, THROWN, VA_LIST only used by wrappers
    }
    else {
        assert(f->source.index == TRASHED_INDEX);
    }

    // If this fires, it means that Flip_Series_To_White was not called an
    // equal number of times after Flip_Series_To_Black, which means that
    // the custom marker on series accumulated.
    //
    assert(TG_Num_Black_Series == 0);

    if (f->gotten != END) {
        assert(IS_WORD(f->value)); // may not match eval_type at this point
        assert(Get_Opt_Var_May_Fail(f->value, f->specifier) == f->gotten);
    }

    // We only have a label if we are in the middle of running a function,
    // and if we're not running a function then f->phase should be NULL.
    //
    assert(f->phase == NULL);
    assert(IS_POINTER_TRASH_DEBUG(f->opt_label));

    assert(NOT(IS_TRASH_DEBUG(&f->cell)));

    //=//// ^-- ABOVE CHECKS *ALWAYS* APPLY ///////////////////////////////=//

    if (FRM_AT_END(f))
        return;

    if (NOT_END(f->out) && THROWN(f->out))
        return;

    assert(f->kind == VAL_TYPE(f->value));

    //=//// v-- BELOW CHECKS ONLY APPLY IN EXITS CASE WITH MORE CODE //////=//

    // The eval_type is expected to be calculated already.  Should match
    // f->value, with special exemption for optimized lookback calls
    // coming from Do_Next_In_Subframe_Throws()
    //
    assert(
        (
            f->eval_type == REB_FUNCTION
            && (IS_WORD(f->value) || IS_FUNCTION(f->value))
        )
        || f->eval_type == VAL_TYPE(f->value)
    );

    assert(f->value);
    assert(FRM_HAS_MORE(f));
    assert(NOT(THROWN(f->value)));
    ASSERT_VALUE_MANAGED(f->value);
    assert(f->value != f->out);

    //=//// ^-- ADD CHECKS EARLIER THAN HERE IF THEY SHOULD ALWAYS RUN ////=//
}


//
//  Do_Core_Expression_Checks_Debug: C
//
// The iteration preamble takes care of clearing out variables and preparing
// the state for a new "/NEXT" evaluation.  It's a way of ensuring in the
// debug build that one evaluation does not leak data into the next, and
// making the code shareable allows code paths that jump to later spots
// in the switch (vs. starting at the top) to reuse the work.
//
void Do_Core_Expression_Checks_Debug(REBFRM *f) {

    assert(f == FS_TOP); // should be topmost frame, still

    Do_Core_Shared_Checks_Debug(f);

    // The only thing the evaluator can take for granted between evaluations
    // about the output cell is that it's not trash.  In the debug build,
    // give this more teeth by explicitly setting it to an unreadable blank,
    // but only if it wasn't an END marker (that's how we can tell no
    // evaluations have been done yet, consider `(comment [...] + 2)`)
    //
    assert(NOT(IS_TRASH_DEBUG(f->out)));
    if (NOT(IS_UNREADABLE_IF_DEBUG(f->out)) && NOT_END(f->out))
        Init_Unreadable_Blank(f->out);

    // Once a throw is started, no new expressions may be evaluated until
    // that throw gets handled.
    //
    assert(IS_UNREADABLE_IF_DEBUG(&TG_Thrown_Arg));

    // Make sure `cell` is reset in debug build if not doing a `reevaluate`
    // (once this was used by EVAL the native, but now it's used by rebEval()
    // at the API level, which currently sets `f->value = &f->cell;`)
    //
#if !defined(NDEBUG)
    if (f->value != &f->cell)
        Init_Unreadable_Blank(&f->cell);
#endif

    // Trash call variables in debug build to make sure they're not reused.
    // Note that this call frame will *not* be seen by the GC unless it gets
    // chained in via a function execution, so it's okay to put "non-GC safe"
    // trash in at this point...though by the time of that call, they must
    // hold valid values.

    TRASH_POINTER_IF_DEBUG(f->param);
    TRASH_POINTER_IF_DEBUG(f->arg);
    TRASH_POINTER_IF_DEBUG(f->refine);

    TRASH_POINTER_IF_DEBUG(f->args_head);
    TRASH_POINTER_IF_DEBUG(f->varlist);

    TRASH_POINTER_IF_DEBUG(f->original);
    TRASH_POINTER_IF_DEBUG(f->binding);

    // Mutate va_list sources into arrays at fairly random moments in the
    // debug build.  It should be able to handle it at any time.
    //
    if (FRM_IS_VALIST(f) && SPORADICALLY(50)) {
        const REBOOL truncated = TRUE;
        Reify_Va_To_Array_In_Frame(f, truncated);
    }
}


//
//  Do_Core_Exit_Checks_Debug: C
//
void Do_Core_Exit_Checks_Debug(REBFRM *f) {
    Do_Core_Shared_Checks_Debug(f);

    if (NOT(FRM_AT_END(f)) && NOT(FRM_IS_VALIST(f))) {
        assert(
            (f->source.index <= ARR_LEN(f->source.array))
            || (
                (
                    (f->source.pending && IS_END(f->source.pending))
                    || THROWN(f->out)
                )
                && f->source.index == ARR_LEN(f->source.array) + 1
            )
        );
    }

    if (f->flags.bits & DO_FLAG_TO_END)
        assert(THROWN(f->out) || FRM_AT_END(f));

    // Function execution should have written *some* actual output value.
    // checking the VAL_TYPE() is enough to make sure it's not END or trash
    //
    assert(VAL_TYPE(f->out) <= REB_MAX_VOID);

    if (NOT(THROWN(f->out)))
        ASSERT_VALUE_MANAGED(f->out);
}

#endif
