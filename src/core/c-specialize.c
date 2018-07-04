//
//  File: %c-specialize.c
//  Summary: "function related datatypes"
//  Section: datatypes
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2015-2018 Rebol Open Source Contributors
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
// A specialization is an ACTION! which has some of its parameters fixed.
// e.g. `ap10: specialize 'append [value: 5 + 5]` makes ap10 have all the same
// refinements available as APPEND, but otherwise just takes one series arg,
// as it will always be appending 10.
//
// The method used is to store a FRAME! in the specialization's ACT_BODY.
// It contains non-null values for any arguments that have been specialized.
// Do_Core() heeds these when walking the parameters (see `f->special`),
// and processes slots with voids in them normally.
//
// Code is shared between the SPECIALIZE native and specialization of a
// GET-PATH! via refinements, such as `adp: :append/dup/part`.  However,
// specifying a refinement without all its arguments is made complicated
// because ordering matters:
//
//     foo: func [/ref1 arg1 /ref2 arg2 /ref3 arg3] [...]
//
//     foo23: :foo/ref2/ref3
//     foo32: :foo/ref3/ref2
//
//     foo23 A B ;-- should give A to arg2 and B to arg3
//     foo32 A B ;-- should give B to arg2 and A to arg3
//
// Merely filling in the slots for the refinements specified with TRUE will
// not provide enough information for a call to be able to tell the difference
// between the intents.  Also, a call to `foo23/ref1 A B C` does not want to
// make arg1 A, because it should act like `foo/ref2/ref3/ref1 A B C`.
//
// The current trick for solving this efficiently involves exploiting the
// fact that refinements in exemplar frames are nominally only unspecialized
// (null), in use (LOGIC! true) or disabled (LOGIC! false).  So a REFINEMENT!
// is put in refinement slots that aren't fully specialized, to give a partial
// that should be pushed to the top of the list of refinements in use.
//
// Mechanically it's "simple", but may look a little counterintuitive.  These
// words are appearing in refinement slots that they don't have any real
// correspondence to.  It's just that they want to be able to pre-empt those
// refinements from fulfillment, while pushing to the in-use-refinements stack
// in reverse order given in the specialization.
//
// More concretely, the exemplar frame slots for `foo23: :foo/ref2/ref3` are:
//
// * REF1's slot would contain the REFINEMENT! ref3.  As Do_Core() traverses
//   the arguments it pushes ref3 to be the current first-in-line to take
//   arguments at the callsite.  Yet REF1 has not been "specialized out", so
//   a call like `foo23/ref1` is legal...it's just that pushing ref3 from the
//   ref1 slot means ref1 defers gathering arguments at the callsite.
//
// * REF2's slot would contain the REFINEMENT! ref2.  This will push ref2 to
//   now be first in line in fulfillment.
//
// * REF3's slot would hold a null, having the typical appearance of not
//   being specialized.
//

#include "sys-core.h"


//
//  Make_Context_For_Action_Int_Partials: C
//
// This creates a FRAME! context with "Nulled" in all the unspecialized slots
// that are available to be filled.  For partial refinement specializations
// in the action, it will push the refinement to the stack and fill the arg
// slot in the new context with an INTEGER! indicating the data stack
// position of the partial.  In this way it retains the ordering information
// implicit in the refinements of an action's existing specialization.
//
// It is able to take in more specialized refinements on the stack.  These
// will be ordered *after* partial specializations in the function already.
// The caller passes in the stack pointer of the lowest priority refinement,
// which goes up to DSP for the highest of those added specializations.
//
// Since this is walking the parameters to make the frame already--and since
// we don't want to bind to anything specialized out (including the ad-hoc
// refinements added on the stack) we go ahead and collect bindings from the
// frame if needed.
//
// Note: For added refinements, as with any other parameter specialized out,
// the bindings are not added at all, vs. some kind of error...
//
//     specialize 'append/dup [dup: false] ; Note DUP: isn't frame /DUP
//
REBCTX *Make_Context_For_Action_Int_Partials(
    const REBVAL *action, // need ->binding, so can't just be a REBACT*
    REBDSP lowest_ordered_dsp, // caller can add refinement specializations
    struct Reb_Binder *opt_binder,
    REBFLGS prep // cell formatting mask bits, result managed if non-stack
){
    REBDSP highest_ordered_dsp = DSP;

    REBACT *act = VAL_ACTION(action);

    // See LINK().facade for details.  [0] cell is underlying function, then
    // there is a parameter for each slot, possibly hidden by specialization.
    //
    // We manage the series even though it is incomplete during this routine.
    // No code runs that can start the GC, so the incompleteness should be ok.
    //
    REBCNT num_slots = ACT_FACADE_NUM_PARAMS(act) + 1;
    REBARR *facade = Make_Array_Core(
        num_slots,
        (SERIES_MASK_ACTION & ~ARRAY_FLAG_PARAMLIST) // [0] is not archetype
    );

    REBVAL *rootkey = SINK(ARR_HEAD(facade));
    Init_Action_Unbound(rootkey, ACT_UNDERLYING(act));

    REBARR *varlist = Make_Array_Core(
        num_slots, // includes +1 for the CTX_ARCHETYPE() at [0]
        SERIES_MASK_CONTEXT
    );

    REBVAL *rootvar = SINK(ARR_HEAD(varlist));
    RESET_VAL_HEADER(rootvar, REB_FRAME);
    rootvar->payload.any_context.varlist = varlist;
    rootvar->payload.any_context.phase = VAL_ACTION(action);
    INIT_BINDING(rootvar, VAL_BINDING(action));

    // Copy values from any prior specializations, transforming REFINEMENT!
    // used for partial specializations into INTEGER! or null, depending
    // on whether that slot was actually specialized out.

    REBVAL *alias = rootkey + 1;
    REBVAL *arg = rootvar + 1;

    const REBVAL *param = ACT_FACADE_HEAD(act);

    REBCTX *exemplar = ACT_EXEMPLAR(act); // may be null
    const REBVAL *special = ACT_SPECIALTY_HEAD(act); // exemplar/facade head
    if (exemplar)
        assert(special == CTX_VARS_HEAD(exemplar));
    else
        assert(special == ACT_FACADE_HEAD(act));

    REBCNT index = 1;

    for (; NOT_END(param); ++param, ++arg, ++special, ++alias, ++index) {
        arg->header.bits = prep;

        Move_Value(alias, param); // only change if in passed-in ordering

        if (VAL_PARAM_CLASS(param) != PARAM_CLASS_REFINEMENT) {
            if (special == param) // e.g. exemplar == NULL
                Init_Nulled(arg);
            else
                Move_Value(arg, special);

            if (opt_binder) {
                REBSTR *canon = VAL_PARAM_CANON(param);
                if (NOT_VAL_FLAG(param, TYPESET_FLAG_UNBINDABLE))
                    Add_Binder_Index(opt_binder, canon, index);
            }

            continue;
        }

        if (special != param) { // e.g. exemplar != NULL
            if (IS_LOGIC(special)) { // guaranteed used, or fully disabled
                Init_Logic(arg, VAL_LOGIC(special));
                goto continue_unbindable;
            }

            if (IS_NULLED(special)) {
                //
                // Might find it on the stack
            }
            else {
                assert(IS_REFINEMENT(special));

                // Save to the stack (they're in *reverse* order of use).
                //
                // !!! Review potential use of Move_Value() here.
                //
                REBCNT partial_index = VAL_WORD_INDEX(special);
                DS_PUSH_TRASH;
                Init_Any_Word_Bound(
                    DS_TOP,
                    REB_REFINEMENT,
                    VAL_STORED_CANON(special),
                    exemplar,
                    partial_index
                );

                if (partial_index <= index) {
                    //
                    // We've already passed the slot we need to mark partial.
                    // Go back and fill it in, and consider the stack item
                    // to be completed/bound
                    //
                    REBVAL *passed = rootvar + partial_index;
                    assert(passed->header.bits == prep);

                    assert(
                        VAL_STORED_CANON(special) ==
                        VAL_PARAM_CANON(
                            CTX_KEYS_HEAD(exemplar) + partial_index - 1
                        )
                    );

                    Init_Integer(passed, DSP);

                    if (partial_index == index)
                        goto continue_unbindable; // just filled in this slot
                }
            }

            if (IS_REFINEMENT_SPECIALIZED(param)) {
                //
                // We know this is partial (and should be set to an INTEGER!)
                // but it may have been pushed to the stack already, or it may
                // be coming along later.  Search only the higher priority
                // pushes since the call began.
                //
                REBDSP dsp = DSP;
                for (; dsp != highest_ordered_dsp; --dsp) {
                    REBVAL *ordered = DS_AT(dsp);
                    assert(IS_WORD_BOUND(ordered));
                    if (VAL_WORD_INDEX(ordered) == index) { // prescient push
                        assert(
                            VAL_PARAM_CANON(param)
                            == VAL_STORED_CANON(ordered)
                        );

                        Init_Integer(arg, dsp);
                        goto continue_unbindable;
                    }
                }

                assert(arg->header.bits == prep); // fill in above later.
                goto continue_unbindable;
            }
        }

        // If we get here, then the refinement is unspecified in the
        // exemplar, *but* the passed in refinements may wish to override
        // that in a "virtual" sense...and remove it from binding
        // consideration for a specialization, e.g.
        //
        //     specialize 'append/only [only: false] ; won't disable only

        assert(not IS_REFINEMENT_SPECIALIZED(param));

        REBSTR *param_canon; // goto would cross initialization
        param_canon = VAL_PARAM_CANON(param);

        REBDSP dsp;
        for (dsp = highest_ordered_dsp; dsp != lowest_ordered_dsp; --dsp) {
            REBVAL *ordered = DS_AT(dsp);
            if (VAL_STORED_CANON(ordered) == param_canon) {
                assert(not IS_WORD_BOUND(ordered)); // we bind only one
                INIT_BINDING(ordered, varlist);
                ordered->payload.any_word.index = index;

                // Wasn't hidden in the incoming paramlist, but it should be
                // hidden from the user when they are running their code
                // bound into this frame--even before the specialization
                // based on the outcome of that code has been calculated.
                //
                Init_Integer(arg, dsp);
                SET_VAL_FLAGS(
                    alias, TYPESET_FLAG_HIDDEN | TYPESET_FLAG_UNBINDABLE
                );
                goto continue_unbindable;
            }
        }

        // void and has no known order, so unspecified/bindable...we have to
        // make it a void for now, because this slot will be seen by the user
        //
        Init_Nulled(arg);
        if (opt_binder)
            Add_Binder_Index(opt_binder, param_canon, index);

    continue_unbindable:;

        continue;
    }

    TERM_ARRAY_LEN(varlist, num_slots);
    MISC(varlist).meta = NULL; // GC sees this, we must initialize

    // !!! Can't currently pass SERIES_FLAG_STACK into Make_Array_Core(),
    // because TERM_ARRAY_LEN won't let it set stack array lengths.
    //
    if (prep & CELL_FLAG_STACK)
        SET_SER_FLAG(varlist, SERIES_FLAG_STACK);

    // This facade is not final--when code runs bound into this context, it
    // might wind up needing to hide more fields.  But also, it will be
    // patched out and in as the function's facade, with the unspecialized
    // keylist taking its place, at the end of specialization.
    //
    TERM_ARRAY_LEN(facade, num_slots);
    MANAGE_ARRAY(facade);
    INIT_CTX_KEYLIST_SHARED(CTX(varlist), facade);

    return CTX(varlist);
}


//
//  Make_Context_For_Action: C
//
// This version of context making will consolidate any partial refinements
// back into the varlist, e.g. for MAKE FRAME! which does not intend to call
// Do_Core() on it or weave the pushed refinements in to build a further
// specialization.  It balances the stack while doing consolidation.
//
// !!! It would be more efficient to do this in one pass, but this helps
// keep the phases more clear, in what is kind of tricky code.
//
REBCTX *Make_Context_For_Action(
    const REBVAL *action, // need ->binding, so can't just be a REBACT*
    REBDSP lowest_ordered_dsp,
    struct Reb_Binder *opt_binder
){
    REBCTX *exemplar = Make_Context_For_Action_Int_Partials(
        action,
        lowest_ordered_dsp,
        opt_binder,
        CELL_MASK_NON_STACK
    );

    // Currently this has to be managed because references to it are being
    // used in bindings with indefinite lifetime for partial refinements.
    // As it's only used by MAKE FRAME! right now--which would manage it
    // anyway--this is not a problem.
    //
    MANAGE_ARRAY(CTX_VARLIST(exemplar));

    // Go through the partially specialized and unspecialized refinement slots
    // and move the stack-pushed refinements into them in order from lowest
    // to highest, so when pushed they will have the highest priority (first
    // or deepest use) refinements on top at the end.  See notes at the
    // top of %c-specialize.c for how this strategy works.
    //
    // !!! The process used by SPECIALIZE is more nuanced, and will actually
    // notice when a refinement has no arguments and thus turn it into a
    // LOGIC! true instead of requiring Do_Core() to push it, which is a bit
    // more efficient on the calling side.  Review that point if looking to
    // re-engineer these routines.

    if (DSP == lowest_ordered_dsp)
        return exemplar; // no partial (or potentially partial) slots

    REBVAL *param = CTX_KEYS_HEAD(exemplar);
    REBVAL *arg = CTX_VARS_HEAD(exemplar);
    REBDSP dsp = lowest_ordered_dsp;
    for (; NOT_END(param); ++param, ++arg) {
        if (NOT_VAL_FLAG(param, TYPESET_FLAG_UNBINDABLE))
            continue; // unspecialized
        if (VAL_PARAM_CLASS(param) != PARAM_CLASS_REFINEMENT)
            continue; // possibly specialized, but not a refinement
        if (IS_LOGIC(arg))
            continue; // fully specialized refinement

        // NOTE: INTEGER! here represents specialized refinement, while NULL
        // represents an unspecialized one.  After the filling process below
        // this distinction is rediscoverable, but it would be easy to cache
        // it here (e.g. with NODE_FLAG_MARKED on the arg) if that were
        // deemed useful for some reason.
        //
        assert(IS_NULLED(arg) or IS_INTEGER(arg));

        if (dsp == DSP) {
            Init_Nulled(arg); // have to overwrite any INTEGER! slots
            continue;
        }

        ++dsp;
        REBVAL *ordered = DS_AT(dsp);
        assert(IS_REFINEMENT(ordered));
        assert(
            VAL_WORD_SPELLING(ordered) ==
            VAL_PARAM_SPELLING(CTX_KEY(exemplar, VAL_WORD_INDEX(ordered)))
        );

        // Binding in ordered is to exemplar, arg is a stack cell... hence
        // the exemplar must be managed for this to be legal (as it is not
        // SERIES_FLAG_STACK, and doesn't do on-demand management).
        //
        Move_Value(arg, ordered);
    }
    assert(dsp == DSP); // should have handled everything

    DS_DROP_TO(lowest_ordered_dsp);
    return exemplar;
}


// On REB_0_PARTIALs, the NODE_FLAG_MARKED is used to keep track of if a
// void argument for that partial is ever seen.  If it never gets set, then
// the partial refinement was actually fulfilled.

inline static void Mark_Void_Arg_Seen(REBVAL *p) {
    assert(VAL_TYPE(p) == REB_0_PARTIAL);
    SET_VAL_FLAG(p, NODE_FLAG_MARKED);
}

inline static REBOOL Saw_Void_Arg_Of(const REBVAL *p) {
    assert(VAL_TYPE(p) == REB_0_PARTIAL);
    return GET_VAL_FLAG(p, NODE_FLAG_MARKED);
}


// Each time we transition the refine field we need to check to see if a
// partial became fulfilled, and if so transition it to not being put into
// the partials.  Better to do it with a macro than repeat the code.  :-/
//
#define FINALIZE_REFINE_IF_FULFILLED \
    assert(evoked != refine or evoked->payload.partial.dsp == 0); \
    if (VAL_TYPE(refine) == REB_0_PARTIAL) { \
        if (not Saw_Void_Arg_Of(refine)) { /* no voids, no order needed! */ \
            if (refine->payload.partial.dsp != 0) \
                Init_Blank(DS_AT(refine->payload.partial.dsp)); /* full! */ \
            else if (refine == evoked) \
                evoked = NULL; /* allow another evoke to be last partial! */ \
        } \
    }


//
//  Specialize_Action_Throws: C
//
// Create a new ACTION! value that uses the same implementation as another,
// but just takes fewer arguments or refinements.  It does this by storing a
// heap-based "exemplar" FRAME! in the specialized action; this stores the
// values to preload in the stack frame cells when it is invoked.
//
// The caller may provide information on the order in which refinements are
// to be specialized, using the data stack.  These refinements should be
// pushed in the *reverse* order of their invocation, so append/dup/part
// has /DUP at DS_TOP, and /PART under it.  List stops at lowest_ordered_dsp.
//
REBOOL Specialize_Action_Throws(
    REBVAL *out,
    REBVAL *specializee,
    REBSTR *opt_specializee_name,
    REBVAL *opt_def, // !!! REVIEW: binding modified directly (not copied)
    REBDSP lowest_ordered_dsp
){
    assert(out != specializee);

    struct Reb_Binder binder;
    if (opt_def != NULL)
        INIT_BINDER(&binder);

    REBACT *unspecialized = VAL_ACTION(specializee);

    // This produces a context where partially specialized refinement slots
    // will be REFINEMENT! pointing into the stack at the partial order
    // position. (This takes into account any we are adding "virtually", from
    // the current DSP down to the lowest_ordered_dsp).
    //
    // Note that REB_0_PARTIAL can't be used in slots yet, because the GC
    // will be able to see this frame (code runs bound into it).
    //
    REBCTX *exemplar = Make_Context_For_Action_Int_Partials(
        specializee,
        lowest_ordered_dsp,
        opt_def ? &binder : nullptr,
        CELL_MASK_NON_STACK
    );
    MANAGE_ARRAY(CTX_VARLIST(exemplar)); // destined to be managed, guarded

    if (opt_def) { // code that fills the frame...fully or partially
        //
        // Bind all the SET-WORD! in the body that match params in the frame
        // into the frame.  This means `value: value` can very likely have
        // `value:` bound for assignments into the frame while `value` refers
        // to whatever value was in the context the specialization is running
        // in, but this is likely the more useful behavior.
        //
        // !!! This binds the actual arg data, not a copy of it--following
        // OBJECT!'s lead.  However, ordinary functions make a copy of the
        // body they are passed before rebinding.  Rethink.

        // See Bind_Values_Core() for explanations of how the binding works.
        assert(GET_SER_FLAG(exemplar, ARRAY_FLAG_VARLIST));
        Bind_Values_Inner_Loop(
            &binder,
            VAL_ARRAY_AT(opt_def),
            exemplar,
            FLAGIT_KIND(REB_SET_WORD), // types to bind (just set-word!)
            0, // types to "add midstream" to binding as we go (nothing)
            BIND_DEEP
        );

        // !!! Only one binder can be in effect, and we're calling arbitrary
        // code.  Must clean up now vs. in loop we do at the end.  :-(
        //
        RELVAL *key = CTX_KEYS_HEAD(exemplar); // the new facade
        REBVAL *var = CTX_VARS_HEAD(exemplar);
        for (; NOT_END(key); ++key, ++var) {
            if (GET_VAL_FLAG(key, TYPESET_FLAG_UNBINDABLE))
                continue; // may be refinement from stack, now specialized out
            Remove_Binder_Index(&binder, VAL_KEY_CANON(key));
        }
        SHUTDOWN_BINDER(&binder);

        // Run block and ignore result (unless it is thrown)
        //
        PUSH_GUARD_CONTEXT(exemplar);
        if (Do_Any_Array_At_Throws(out, opt_def)) {
            DROP_GUARD_CONTEXT(exemplar);
            DS_DROP_TO(lowest_ordered_dsp);
            return true;
        }
        DROP_GUARD_CONTEXT(exemplar);
    }

    REBVAL *rootkey = CTX_ROOTKEY(exemplar);

    // Build up the paramlist for the specialized function on the stack, and
    // fill in the facade slots with whether arguments are speciailzed.  The
    // same walk used for that is used to link and process the REB_0_PARTIAL
    // arguments for whether they become fully specialized or not.

    REBDSP dsp_paramlist = DSP;
    DS_PUSH(ACT_ARCHETYPE(unspecialized));

    REBVAL *param = rootkey + 1;
    REBVAL *arg = CTX_VARS_HEAD(exemplar);
    REBVAL *refine = ORDINARY_ARG; // parallels state logic in Do_Core()
    REBCNT index = 1;

    REBVAL *first_partial = NULL;
    REBVAL *last_partial = NULL;

    REBVAL *evoked = NULL;

    for (; NOT_END(param); ++param, ++arg, ++index) {
        switch (VAL_PARAM_CLASS(param)) {
        case PARAM_CLASS_REFINEMENT: {
            FINALIZE_REFINE_IF_FULFILLED; // see macro
            refine = arg;

            if (
                IS_NULLED(refine)
                or (IS_INTEGER(refine) and IS_REFINEMENT_SPECIALIZED(param))
            ){
                //
                // /DUP is implicitly "evoked" to be true in the following
                // case, despite being void, since an argument is supplied:
                //
                //     specialize 'append [count: 10]
                //
                // But refinements with one argument that get evoked might
                // cause partial refinement specialization.  Since known
                // partials are checked to see if they become complete anyway,
                // use the same mechanic for voids.

                REBDSP partial_dsp = IS_NULLED(refine) ? 0 : VAL_INT32(refine);

                if (first_partial == NULL)
                    first_partial = refine;
                else
                    last_partial->extra.next_partial = refine;

                RESET_VAL_CELL(refine, REB_0_PARTIAL, 0);
                refine->payload.partial.dsp = partial_dsp;
                refine->payload.partial.index = index;
                TRASH_POINTER_IF_DEBUG(refine->extra.next_partial);

                last_partial = refine;

                if (partial_dsp == 0)
                    goto unspecialized_arg_but_may_evoke;

                // Though Make_Frame_For_Specialization() knew this slot was
                // partial when it ran, user code might have run to fill in
                // all the void arguments.  We need to know the stack position
                // of the ordering, to BLANK! it from the partial stack if so.
                //
                goto specialized_arg_no_typecheck;
            }
            else if (IS_LOGIC(refine))
                goto specialized_arg_no_typecheck;

            fail (Error_Non_Logic_Refinement(param, refine)); }

        case PARAM_CLASS_RETURN_1:
        case PARAM_CLASS_RETURN_0:
        case PARAM_CLASS_LOCAL:
            assert(IS_NULLED(arg)); // no bindings, you can't set these
            goto unspecialized_arg;

        default:
            break;
        }

        // It's an argument, either a normal one or a refinement arg.

        if (refine == ORDINARY_ARG) {
            if (IS_NULLED(arg))
                goto unspecialized_arg;
            goto specialized_arg;
        }

        if (VAL_TYPE(refine) == REB_0_PARTIAL) {
            if (IS_NULLED(arg)) {
                Mark_Void_Arg_Seen(refine); // we *know* it's not fulfilled
                goto unspecialized_arg;
            }

            if (refine->payload.partial.dsp != 0) // started true
                goto specialized_arg;

            if (evoked == refine)
                goto specialized_arg; // already evoking this refinement

            // If we started out with a void refinement this arg "evokes" it.
            // (Opposite of void "revocation" at callsites).
            // An "evoked" refinement from the code block has no order,
            // so only one such partial is allowed, unless it turns out to
            // be completely fulfilled.
            //
            if (evoked != NULL)
                fail (Error_Ambiguous_Partial_Raw());

            REBVAL *fix = param - (arg - refine); // specialize in facade
            SET_VAL_FLAGS(fix, TYPESET_FLAG_HIDDEN | TYPESET_FLAG_UNBINDABLE);

            assert(VAL_PARAM_CLASS(DS_TOP) == PARAM_CLASS_REFINEMENT);
            assert(VAL_PARAM_SPELLING(DS_TOP) == VAL_PARAM_SPELLING(fix));
            DS_DROP; // added at `unspecialized_but_may_evoke`, but drop it

            evoked = refine; // gets reset to NULL if ends up fulfilled
            goto specialized_arg;
        }

        assert(IS_LOGIC(refine));

        if (VAL_LOGIC(refine) == false) {
            //
            // `specialize 'append [dup: false count: 10]` is not legal.
            //
            if (not IS_NULLED(arg))
                fail (Error_Bad_Refine_Revoke(param, arg));
            goto specialized_arg_no_typecheck;
        }

        if (not IS_NULLED(arg))
            goto unspecialized_arg;

        // A previously fully-specialized TRUE should not have any void args.
        // But code run for the specialization may have set the refinement
        // to true without setting all its arguments.
        //
        // Unlike with the REB_0_PARTIAL cases, we have no ordering info
        // besides "after all of those", we can only do that *once*.

        if (evoked != NULL)
            fail (Error_Ambiguous_Partial_Raw());

        // Link into partials list (some repetition with code above)

        if (first_partial == NULL)
            first_partial = refine;
        else
            last_partial->extra.next_partial = refine;

        RESET_VAL_CELL(refine, REB_0_PARTIAL, 0);
        refine->payload.partial.dsp = 0; // no ordered position on stack
        refine->payload.partial.index = index - (arg - refine);
        TRASH_POINTER_IF_DEBUG(refine->extra.next_partial);

        last_partial = refine;

        Mark_Void_Arg_Seen(refine); // void arg, so can't be completely filled
        evoked = refine; // ...so we won't ever set this back to NULL later
        goto unspecialized_arg;

    unspecialized_arg_but_may_evoke:;

        assert(refine->payload.partial.dsp == 0);
        assert(not IS_REFINEMENT_SPECIALIZED(param));

    unspecialized_arg:;

        DS_PUSH(param); // if evoked, will get DS_DROP'd from the paramlist
        continue;

    specialized_arg:;

        assert(VAL_PARAM_CLASS(param) != PARAM_CLASS_REFINEMENT);
        if (GET_VAL_FLAG(param, TYPESET_FLAG_UNBINDABLE)) {
            //
            // Argument was previously specialized, should have been type
            // checked already.
            //
            assert(GET_VAL_FLAG(param, TYPESET_FLAG_HIDDEN));
        }
        else {
            if (GET_VAL_FLAG(param, TYPESET_FLAG_VARIADIC))
                fail ("Cannot currently SPECIALIZE variadic arguments.");

            if (not TYPE_CHECK(param, VAL_TYPE(arg)))
                fail (Error_Invalid(arg)); // !!! merge w/Error_Invalid_Arg()
        }

    specialized_arg_no_typecheck:;

        SET_VAL_FLAGS(param, TYPESET_FLAG_HIDDEN | TYPESET_FLAG_UNBINDABLE);
        continue;
    }

    if (first_partial != NULL) {
        FINALIZE_REFINE_IF_FULFILLED; // last chance (no more refinements)
        last_partial->extra.next_partial = NULL; // not needed until now
    }

    REBARR *paramlist = Pop_Stack_Values_Core(
        dsp_paramlist,
        SERIES_MASK_ACTION
    );
    MANAGE_ARRAY(paramlist);
    RELVAL *rootparam = ARR_HEAD(paramlist);
    rootparam->payload.action.paramlist = paramlist;

    // The exemplar frame slots now contain a linked list of REB_0_PARTIAL
    // slots.  These slots need to be converted into TRUE if they are actually
    // fully fulfilled, REFINEMENT! to hold partial refinements in the reverse
    // order of their application, or void when partials have run out.
    //
    // (Note that the result may have voids in slots that are "actually" true,
    // and a REFINEMENT! can appear in slots that aren't specialized at all.
    // However, one can tell if a refinement is true by looking in the facade
    // and seeing if it has a bit saying it was "specialized out".)
    //
    // !!! If (dsp == DSP), e.g. no partials pending after this one, and
    // the parameter's canon matches the partial's canon its storing...we
    // *could* signal to the evaluator that it could consume the slot
    // without a pickup pass, e.g. by leaving the refinement unbound.
    // For now, avoid doing that, as it's confusing (and likely rare).
    //
    REBVAL *partial = first_partial;
    REBDSP dsp = lowest_ordered_dsp;
    while (partial != NULL) {
        assert(VAL_TYPE(partial) == REB_0_PARTIAL);
        REBVAL *next_partial = partial->extra.next_partial; // overwritten

        if (not Saw_Void_Arg_Of(partial)) {
            REBCNT partial_index = partial->payload.partial.index;
            if (IS_REFINEMENT_SPECIALIZED(rootkey + partial_index)) {
                //
                // Since it's not revealed in the facade, it must be in use
                // (even if it was "evoked" to be so, from void).  If all the
                // args were non-void, it's not actually a partial.
                //
                Init_Logic(partial, true);
                goto continue_loop;
            }
        }

        if (evoked != NULL) {
            //
            // A non-position-bearing refinement use coming from running the
            // code block will come after all the refinements in the path,
            // making it first in the exemplar partial/unspecialized slots.
            //
            REBCNT evoked_index = evoked->payload.partial.index;
            assert(IS_REFINEMENT_SPECIALIZED(rootkey + evoked_index));

            Init_Any_Word_Bound(
                partial,
                REB_REFINEMENT,
                VAL_PARAM_CANON(rootkey + evoked_index),
                exemplar,
                evoked_index
            );

            evoked = NULL;
            goto continue_loop;
        }

    try_higher_ordered:;

        if (dsp != DSP) {
            ++dsp;
            REBVAL *ordered = DS_AT(dsp);
            if (IS_BLANK(ordered)) // blanked when seen to be longer partial
                goto try_higher_ordered;
            if (IS_WORD_UNBOUND(ordered)) // not in paramlist, or a duplicate
                fail (Error_Bad_Refine_Raw(ordered));

            Init_Any_Word_Bound(
                partial,
                REB_REFINEMENT,
                VAL_STORED_CANON(ordered),
                exemplar,
                VAL_WORD_INDEX(ordered)
            );

            goto continue_loop;
        }

        Init_Nulled(partial);

    continue_loop:;

        partial = next_partial;
    }

    // If there was no error, everything should have balanced out...and if
    // there's anything left on the stack, it should be a ordered refinement
    // that was completely fulfilled and hence blank.
    //
    assert(evoked == NULL);
    while (dsp != DSP) {
        ++dsp;
        REBVAL *ordered = DS_AT(dsp);
        if (not IS_BLANK(ordered))
            fail (Error_Bad_Refine_Raw(ordered)); // specialize 'print/asdf
    }
    DS_DROP_TO(lowest_ordered_dsp);

    // See %sysobj.r for `specialized-meta:` object template

    REBVAL *example = Get_System(SYS_STANDARD, STD_SPECIALIZED_META);

    REBCTX *meta = Copy_Context_Shallow(VAL_CONTEXT(example));

    Init_Nulled(CTX_VAR(meta, STD_SPECIALIZED_META_DESCRIPTION)); // default
    Move_Value(
        CTX_VAR(meta, STD_SPECIALIZED_META_SPECIALIZEE),
        specializee
    );
    if (opt_specializee_name == NULL)
        Init_Nulled(CTX_VAR(meta, STD_SPECIALIZED_META_SPECIALIZEE_NAME));
    else
        Init_Word(
            CTX_VAR(meta, STD_SPECIALIZED_META_SPECIALIZEE_NAME),
            opt_specializee_name
        );

    MANAGE_ARRAY(CTX_VARLIST(meta));
    MISC(paramlist).meta = meta;

    REBARR *facade = CTX_KEYLIST(exemplar);

    REBACT *specialized = Make_Action(
        paramlist,
        &Specializer_Dispatcher,
        facade, // use facade with specialized parameters flagged hidden
        exemplar // also provide a context of specialization values
    );

    // We patch the facade of the unspecialized action in as the keylist
    // for the frame.  When the frame is molded, it will now show the
    // specialized keys and values (some of which may have been suppressed
    // when the facade was being used as the keylist)
    //
    INIT_CTX_KEYLIST_SHARED(exemplar, ACT_FACADE(unspecialized));

    // The "body" is the FRAME! value of the specialization.  It takes on the
    // binding we want to use (which we can't put in the exemplar archetype,
    // that binding has to be UNBOUND).  It also remembers the original
    // action in the phase, so Specializer_Dispatcher() knows what to call.
    //
    RELVAL *body = ACT_BODY(specialized);
    Move_Value(body, CTX_ARCHETYPE(exemplar));
    INIT_BINDING(body, VAL_BINDING(specializee));
    body->payload.any_context.phase = unspecialized;

    Init_Action_Unbound(out, specialized);
    return false; // code block did not throw
}


//
//  Specializer_Dispatcher: C
//
// The evaluator does not do any special "running" of a specialized frame.
// All of the contribution that the specialization had to make was taken care
// of when Do_Core() used f->special to fill from the exemplar.  So all
// this does is change the phase and binding to match the function that this
// layer was specializing.
//
REB_R Specializer_Dispatcher(REBFRM *f)
{
    REBVAL *exemplar = KNOWN(ACT_BODY(FRM_PHASE(f)));

    FRM_PHASE(f) = exemplar->payload.any_context.phase;
    FRM_BINDING(f) = VAL_BINDING(exemplar);
    return R_REDO_UNCHECKED; // redo uses the updated phase and binding
}


//
//  specialize: native [
//
//  {Create a new action through partial or full specialization of another}
//
//      return: [action!]
//      specializee [action! word! path!]
//          {Function or specifying word (preserves word name for debug info)}
//      def [block!]
//          {Definition for FRAME! fields for args and refinements}
//  ]
//
REBNATIVE(specialize)
{
    INCLUDE_PARAMS_OF_SPECIALIZE;

    REBVAL *specializee = ARG(specializee);

    REBDSP lowest_ordered_dsp = DSP;

    // Any partial refinement specializations are pushed to the stack, and
    // gives ordering information that TRUE assigned in a code block can't.
    //
    REBSTR *opt_name;
    if (Get_If_Word_Or_Path_Throws(
        D_OUT,
        &opt_name,
        specializee,
        SPECIFIED,
        true // push_refines = true (don't generate temp specialization)
    )){
        // e.g. `specialize 'append/(throw 10 'dup) [value: 20]`
        //
        return D_OUT;
    }

    // Note: Even if there was a PATH! doesn't mean there were refinements
    // used, e.g. `specialize 'lib/append [...]`.

    if (not IS_ACTION(D_OUT))
        fail (Error_Invalid(specializee));
    Move_Value(specializee, D_OUT); // Frees D_OUT, and GC safe (in ARG slot)

    if (Specialize_Action_Throws(
        D_OUT,
        specializee,
        opt_name,
        ARG(def),
        lowest_ordered_dsp
    )){
        // e.g. `specialize 'append/dup [value: throw 10]`
        //
        return D_OUT;
    }

    return D_OUT;
}


//
//  Block_Dispatcher: C
//
// There are no arguments or locals to worry about in a DOES, nor does it
// heed any definitional RETURN.  This means that in many common cases we
// don't need to do anything special to a BLOCK! passed to DO...no copying
// or otherwise.  Just run it when the function gets called.
//
// Yet `does [...]` isn't *quite* like `specialize 'do [source: [...]]`.  The
// difference is subtle, but important when interacting with bindings to
// fields in derived objects.  That interaction cannot currently resolve such
// bindings without a copy, so it is made on demand.
//
// (Luckily these copies are often not needed, such as when the DOES is not
// used in a method... -AND- it only needs to be made once.)
//
REB_R Block_Dispatcher(REBFRM *f)
{
    RELVAL *block = ACT_BODY(FRM_PHASE(f));
    assert(IS_BLOCK(block));

    if (IS_SPECIFIC(block)) {
        if (FRM_BINDING(f) == UNBOUND) {
            if (Do_Any_Array_At_Throws(f->out, KNOWN(block)))
                return f->out;
            return f->out;
        }

        // Until "virtual binding" is implemented, we would lose f->binding's
        // ability to influence any variable lookups in the block if we did
        // not relativize it to this frame.  This is the only current way to
        // "beam down" influence of the binding for cases like:
        //
        // What forces us to copy the block are cases like this:
        //
        //     o1: make object! [a: 10 b: does [if true [a]]]
        //     o2: make o1 [a: 20]
        //     o2/b = 20
        //
        // While o2/b's ACTION! has a ->binding to o2, the only way for the
        // [a] block to get the memo is if it is relative to o2/b.  It won't
        // be relative to o2/b if it didn't have its existing relativism
        // Derelativize()'d out to make it specific, and then re-relativized
        // through a copy on behalf of o2/b.

        REBARR *body_array = Copy_And_Bind_Relative_Deep_Managed(
            KNOWN(block),
            ACT_PARAMLIST(FRM_PHASE(f)),
            TS_ANY_WORD
        );

        // Preserve file and line information from the original, if present.
        //
        if (GET_SER_FLAG(VAL_ARRAY(block), ARRAY_FLAG_FILE_LINE)) {
            LINK(body_array).file = LINK(VAL_ARRAY(block)).file;
            MISC(body_array).line = MISC(VAL_ARRAY(block)).line;
            SET_SER_FLAG(body_array, ARRAY_FLAG_FILE_LINE);
        }

        // Need to do a raw initialization of this block RELVAL because it is
        // relative to a function.  (Init_Block assumes all specific values.)
        //
        INIT_VAL_ARRAY(block, body_array);
        VAL_INDEX(block) = 0;
        INIT_BINDING(block, FRM_PHASE(f)); // relative binding

        // Block is now a relativized copy; we won't do this again.
    }

    assert(IS_RELATIVE(block));

    if (Do_At_Throws(
        f->out,
        VAL_ARRAY(block),
        VAL_INDEX(block),
        SPC(f->varlist)
    )){
        return f->out;
    }

    return f->out;
}


//
//  defer-0: native [
//
//  {<INTERNAL> No-op dispatcher used to avoid a flag check in the eval loop}
//
//  ]
//
REBNATIVE(defer_0)
//
// See `#define FRM_PHASE` for the safety precaution that helps this not be
// mistaken for the actual intended phase of a frame being fulfilled.
{
    Init_Bar(D_OUT);
    return D_OUT;
}


//
//  Make_Invocation_Frame_Throws: C
//
// Logic shared currently by DOES and MATCH to build a single executable
// frame from feeding forward a VARARGS! parameter, which is a bit like being
// able to call DO/NEXT via Do_Core() yet introspect the evaluator step.
//
REBOOL Make_Invocation_Frame_Throws(
    REBVAL *out, // in case there is a throw
    REBFRM *f,
    REBVAL **first_arg_ptr, // returned so that MATCH can steal it
    const REBVAL *action,
    REBVAL *varargs,
    REBDSP lowest_ordered_dsp
){
    assert(IS_ACTION(action));
    assert(IS_VARARGS(varargs));

    // !!! The vararg's frame is not really a parent, but try to stay
    // consistent with the naming in subframe code copy/pasted for now...
    //
    REBFRM *parent;
    if (not Is_Frame_Style_Varargs_May_Fail(&parent, varargs))
        fail (
            "Currently MAKE FRAME! on a VARARGS! only works with a varargs"
            " which is tied to an existing, running frame--not one that is"
            " being simulated from a BLOCK! (e.g. MAKE VARARGS! [...])"
        );

    assert(parent->eval_type == REB_ACTION);

    // Slip the REBFRM a dsp_orig which may be lower than the DSP captured by
    // DECLARE_FRAME().  This way, it will see any pushes done during a
    // path resolution as ordered refinements to use.
    //
    f->dsp_orig = lowest_ordered_dsp;

    // === FIRST PART OF CODE FROM DO_SUBFRAME ===
    f->out = out;

    f->source = parent->source;
    f->value = parent->value;
    f->gotten = parent->gotten;
    f->specifier = parent->specifier;
    TRASH_POINTER_IF_DEBUG(parent->gotten);

    // Just do one step of the evaluator, so no DO_FLAG_TO_END.  Specifically,
    // it is desired that any voids encountered be processed as if they are
    // not specialized...and gather at the callsite if necessary.
    //
    Init_Endlike_Header(
        &f->flags,
        DO_FLAG_GOTO_PROCESS_ACTION
    );

    Push_Frame_Core(f);
    Reuse_Varlist_If_Available(f);

    // === END FIRST PART OF CODE FROM DO_SUBFRAME ===

    REBSTR *opt_label = nullptr; // !!! for now
    Push_Action(f, VAL_ACTION(action), VAL_BINDING(action));
    Begin_Action(f, opt_label, ORDINARY_ARG);

    // !!! A hack here is needed to slip in a lie to make the dispatcher not
    // run the action, but rather to throw back to us.
    //
    assert(FRM_BINDING(f) == VAL_BINDING(action));
    assert(FRM_PHASE(f) == VAL_ACTION(action));
    f->rootvar->payload.any_context.phase = NAT_ACTION(defer_0);
    (*PG_Do)(f);
    f->rootvar->payload.any_context.phase = VAL_ACTION(action);
    FRM_BINDING(f) = VAL_BINDING(action); // can change during invoke

    // The function did not actually execute, so no SPC(f) was never handed
    // out...the varlist should never have gotten managed.  So this context
    // can theoretically just be put back into the reuse list, or managed
    // and handed out for other purposes by the caller.
    //
    assert(NOT_SER_FLAG(f->varlist, NODE_FLAG_MANAGED));

    parent->source = f->source;
    parent->value = f->value;
    parent->gotten = f->gotten;
    assert(parent->specifier == f->specifier); // !!! can't change?

    if (f->flags.bits & DO_FLAG_BARRIER_HIT)
        parent->flags.bits |= DO_FLAG_BARRIER_HIT;

    if (THROWN(f->out))
        return true;

    assert(IS_BAR(f->out)); // guaranteed by defer_0, for the skipped action

    // === END SECOND PART OF CODE FROM DO_SUBFRAME ===

    *first_arg_ptr = nullptr;

    REBVAL *refine = nullptr;
    REBVAL *param = CTX_KEYS_HEAD(CTX(f->varlist));
    REBVAL *arg = CTX_VARS_HEAD(CTX(f->varlist));
    for (; NOT_END(param); ++param, ++arg) {
        enum Reb_Param_Class pclass = VAL_PARAM_CLASS(param);
        switch (pclass) {
        case PARAM_CLASS_REFINEMENT:
            refine = param;
            break;

        case PARAM_CLASS_NORMAL:
        case PARAM_CLASS_TIGHT:
        case PARAM_CLASS_HARD_QUOTE:
        case PARAM_CLASS_SOFT_QUOTE:
            if (not refine or VAL_LOGIC(refine)) {
                *first_arg_ptr = arg;
                goto found_first_arg_ptr;
            }
            break;

        case PARAM_CLASS_LOCAL:
        case PARAM_CLASS_RETURN_1:
        case PARAM_CLASS_RETURN_0:
            break;

        default:
            panic ("Unknown PARAM_CLASS");
        }
    }

    fail ("ACTION! has no args to MAKE FRAME! from...");

found_first_arg_ptr:

    // DS_DROP_TO(lowest_ordered_dsp);

    return false;
}


//
//  does: native [
//
//  {Specializes DO for a value (or for args of another named function)}
//
//      return: [action!]
//      'specializee [any-value!]
//          {WORD! or PATH! names function to specialize, else arg to DO}
//      :args [any-value! <...>]
//          {arguments which will be consumed to fulfill a named function}
//  ]
//
REBNATIVE(does)
{
    INCLUDE_PARAMS_OF_DOES;

    REBVAL *specializee = ARG(specializee);

    REBARR *paramlist = Make_Array_Core(
        1, // archetype only...DOES always makes function with no arguments
        SERIES_MASK_ACTION
    );

    REBVAL *archetype = Alloc_Tail_Array(paramlist);
    RESET_VAL_HEADER(archetype, REB_ACTION);
    archetype->payload.action.paramlist = paramlist;
    INIT_BINDING(archetype, UNBOUND);
    TERM_ARRAY_LEN(paramlist, 1);

    LINK(paramlist).facade = paramlist;
    MISC(paramlist).meta = NULL; // REDESCRIBE can be used to add help

    if (IS_BLOCK(specializee)) {
        //
        // `does [...]` and `does do [...]` are not exactly the same.  The
        // generated ACTION! of the first form uses Block_Dispatcher() and
        // does on-demand relativization, so it's "kind of like" a `func []`
        // in forwarding references to members of derived objects.  Also, it
        // is optimized to not run the block with the DO native...hence a
        // HIJACK of DO won't be triggered by invocations of the first form.
        //
        MANAGE_ARRAY(paramlist);
        REBACT *doer = Make_Action(
            paramlist,
            &Block_Dispatcher, // **SEE COMMENTS**, not quite like plain DO!
            NULL, // no facade (use paramlist)
            NULL // no specialization exemplar (or inherited exemplar)
        );

        // Block_Dispatcher() *may* copy at an indeterminate time, so to keep
        // things invariant we have to lock it.
        //
        RELVAL *body = ACT_BODY(doer);
        REBSER *locker = NULL;
        Ensure_Value_Immutable(specializee, locker);
        Move_Value(body, specializee);

        Init_Action_Unbound(D_OUT, doer);
        return D_OUT;
    }

    REBCTX *exemplar;
    if (
        GET_VAL_FLAG(specializee, VALUE_FLAG_UNEVALUATED)
        and (IS_WORD(specializee) or IS_PATH(specializee))
    ){
        REBSTR *opt_label;
        REBDSP lowest_ordered_dsp = DSP;
        if (Get_If_Word_Or_Path_Throws(
            D_OUT,
            &opt_label,
            specializee,
            SPECIFIED,
            true // push_refinements = true
        )){
            return D_OUT;
        }

        if (not IS_ACTION(D_OUT))
            fail (Error_Invalid(specializee));

        Move_Value(specializee, D_OUT);

        // We interpret phrasings like `x: does all [...]` to mean something
        // like `x: specialize 'all [block: [...]]`.  While this originated
        // from the Rebmu code golfing language to eliminate a pair of bracket
        // characters from `x: does [all [...]]`, it actually has different
        // semantics...which can be useful in their own right, plus the
        // resulting function will run faster.

        DECLARE_FRAME (f); // REBFRM whose built FRAME! context we will steal

        REBVAL *first_arg;
        if (Make_Invocation_Frame_Throws(
            D_OUT,
            f,
            &first_arg,
            specializee,
            ARG(args),
            lowest_ordered_dsp
        )){
            return D_OUT;
        }
        assert(NOT_SER_FLAG(f->varlist, NODE_FLAG_MANAGED)); // not invoked yet
        assert(FRM_BINDING(f) == VAL_BINDING(specializee));
        exemplar = Steal_Context_Vars(
            CTX(f->varlist),
            NOD(VAL_ACTION(specializee))
        );
        LINK(exemplar).keysource = NOD(ACT_FACADE(VAL_ACTION(specializee)));
        assert(
            ACT_FACADE_NUM_PARAMS(VAL_ACTION(specializee))
            == CTX_LEN(exemplar)
        );

        SET_SER_FLAG(f->varlist, NODE_FLAG_MANAGED); // is inaccessible
        f->varlist = nullptr; // just let it GC, for now

        Drop_Frame_Core(f); // f->eval_type isn't REB_0, may not be FRM_END

        // The exemplar may or may not be managed as of yet.  We want it
        // managed, but Push_Action() does not use ordinary series creation to
        // make its nodes, so manual ones don't wind up in the tracking list.
        //
        SET_SER_FLAG(exemplar, NODE_FLAG_MANAGED); // can't use Manage_Series

        UNUSED(first_arg);
        UNUSED(opt_label);
    }
    else {
        // On all other types, we just make it act like a specialized call to
        // DO for that value.

        exemplar = Make_Context_For_Action(
            NAT_VALUE(do),
            DSP, // lower dsp would be if we wanted to add refinements
            nullptr // don't set up a binder; just poke specializee in frame
        );
        assert(GET_SER_FLAG(exemplar, NODE_FLAG_MANAGED));
        Move_Value(CTX_VAR(exemplar, 1), specializee);
        Move_Value(specializee, NAT_VALUE(do));
    }

    REBACT *unspecialized = VAL_ACTION(specializee);

    REBCNT num_slots = ACT_FACADE_NUM_PARAMS(unspecialized) + 1;
    REBARR *facade = Make_Array_Core(
        num_slots,
        SERIES_MASK_ACTION & ~ARRAY_FLAG_PARAMLIST // [0] slot isn't archetype
    );
    REBVAL *rootkey = SINK(ARR_HEAD(facade));
    Init_Action_Unbound(rootkey, ACT_UNDERLYING(unspecialized));

    REBVAL *param = ACT_FACADE_HEAD(unspecialized);
    RELVAL *alias = rootkey + 1;
    for (; NOT_END(param); ++param, ++alias) {
        Move_Value(alias, param);
        SET_VAL_FLAGS(
            alias, TYPESET_FLAG_HIDDEN | TYPESET_FLAG_UNBINDABLE
        );
    }

    TERM_ARRAY_LEN(facade, num_slots);
    MANAGE_ARRAY(facade);

    MANAGE_ARRAY(paramlist);

    // This code parallels Specialize_Action_Throws(), see comments there

    REBACT *doer = Make_Action(
        paramlist,
        &Specializer_Dispatcher,
        facade, // no facade, use paramlist
        exemplar // also provide a context of specialization values
    );
    Init_Frame(ACT_BODY(doer), exemplar);

    Init_Action_Unbound(D_OUT, doer);
    return D_OUT;
}
