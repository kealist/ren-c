//
//  File: %n-control.c
//  Summary: "native functions for control flow"
//  Section: natives
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2018 Rebol Open Source Contributors
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
// Control constructs in Ren-C differ from R3-Alpha in some ways:
//
// * If they do not run any branches, the construct returns NULL...which is
//   not an ANY-VALUE! and can't be put in a block or assigned to a variable
//   via SET-WORD! or SET-PATH!.  This is systemically the sign of a "soft
//   failure", and can signal constructs like ELSE to run.
//
// * If a branch *does* run--and that branch evaluation produces a NULL--then
//   conditionals designed to be used with branching (like IF or CASE) will
//   return a VOID! result.  Voids are neither true nor false, and are not
//   friendly to work with (e.g. can't be assigned to a variable via SET-WORD!
//   or SET-PATH!).  Yet they are values and can be put in blocks, so are
//   unlike NULL...constructs like ELSE can realize that a branch was taken
//   and not run their own branch.
//
// * Zero-arity function values used as branches will be executed, and
//   single-arity functions used as branches will also be executed--but passed
//   the value of the triggering condition.  See Run_Branch_Throws().
//
// * There is added checking that a literal block is not used as a condition,
//   to catch common mistakes like `if [x = 10] [...]`.
//

#include "sys-core.h"


//
//  if: native [
//
//  {When TO-LOGIC CONDITION is true, execute branch}
//
//      return: "null if branch not run, otherwise branch result"
//          [<opt> any-value!]
//      condition [<opt> any-value!]
//      branch "If arity-1 ACTION!, receives the evaluated condition"
//          [block! action!]
//  ]
//
REBNATIVE(if)
{
    INCLUDE_PARAMS_OF_IF;

    if (IS_CONDITIONAL_FALSE(ARG(condition)))
        return R_NULL;

    if (Run_Branch_Throws(D_OUT, ARG(condition), ARG(branch)))
        return R_OUT_IS_THROWN;

    Voidify_If_Nulled(D_OUT); // null is reserved for no branch run
    return R_OUT;
}


//
//  if-not: native [
//
//  {When TO-LOGIC CONDITION is false, execute branch}
//
//      return: "null if branch not run, otherwise branch result"
//          [<opt> any-value!]
//      condition [<opt> any-value!]
//      branch [block! action!]
//  ]
//
REBNATIVE(if_not)
{
    INCLUDE_PARAMS_OF_IF_NOT;

    if (IS_CONDITIONAL_TRUE(ARG(condition)))
        return R_NULL;

    if (Run_Branch_Throws(D_OUT, ARG(condition), ARG(branch)))
        return R_OUT_IS_THROWN;

    Voidify_If_Nulled(D_OUT); // null is reserved for no branch run
    return R_OUT;
}


//
//  either: native [
//
//  {Choose a branch to execute, based on TO-LOGIC of the CONDITION value}
//
//      return: [<opt> any-value!]
//      condition [<opt> any-value!]
//      true-branch "If arity-1 ACTION!, receives the evaluated condition"
//          [block! action!]
//      false-branch [block! action!]
//  ]
//
REBNATIVE(either)
//
// Note that EITHER is not a precise synonym for IF...ELSE, because both
// branches are allowed to return null, not just the second.
{
    INCLUDE_PARAMS_OF_EITHER;

    if (Run_Branch_Throws(
        D_OUT,
        ARG(condition),
        IS_CONDITIONAL_TRUE(ARG(condition))
            ? ARG(true_branch)
            : ARG(false_branch)
    )){
        return R_OUT_IS_THROWN;
    }

    return R_OUT;
}


//  Either_Test_Core: C
//
// Note: There was an idea of turning the `test` BLOCK! into some kind of
// dialect.  That was later supplanted by idea of MATCH...which bridges with
// a natural interface to functions like PARSE for providing such dialects.
// This routine is just for basic efficiency behind constructs like ELSE
// that want to avoid frame creation overhead.  So BLOCK! just means typeset.
//
inline static REB_R Either_Test_Core(
    REBVAL *cell, // GC-safe temp cell
    REBVAL *test, // modified
    const REBVAL *arg
){
    switch (VAL_TYPE(test)) {

    case REB_LOGIC: { // test for "truthy" or "falsey"
        //
        // If this is the result of composing together a test with a literal,
        // it may be the *test* that changes...so in effect, we could be
        // "testing the test" on a fixed value.  Allow literal blocks (e.g.
        // use IS_TRUTHY() instead of IS_CONDITIONAL_TRUE())
        //
        return R_FROM_BOOL(VAL_LOGIC(test) == IS_TRUTHY(arg)); }

    case REB_WORD:
    case REB_PATH: {
        //
        // !!! Because we do not push refinements here, this means that a
        // specialized action will be generated if the user says something
        // like `either-test 'foo?/bar x [...]`.  It's possible to avoid
        // this by pushing a frame before the Get_If_Word_Or_Path_Throws()
        // and gathering the refinements on the stack, but a bit more work
        // for an uncommon case...revisit later.
        //
        const REBOOL push_refinements = FALSE;

        REBSTR *opt_label = NULL;
        REBDSP lowest_ordered_dsp = DSP;
        if (Get_If_Word_Or_Path_Throws(
            cell,
            &opt_label,
            test,
            SPECIFIED,
            push_refinements
        )){
            return R_OUT_IS_THROWN;
        }

        assert(lowest_ordered_dsp == DSP); // would have made specialization
        UNUSED(lowest_ordered_dsp);

        Move_Value(test, cell);

        if (not IS_ACTION(test))
            fail ("EITHER-TEST only takes WORD! and PATH! for ACTION! vars");
        goto handle_action; }

    case REB_ACTION: {

    handle_action:;

        if (Apply_Only_Throws(
            cell,
            TRUE, // `fully` (ensure argument consumed)
            test,
            NULLIZE(arg), // convert nulled cells to C nullptr for API
            END
        )){
            return R_OUT_IS_THROWN;
        }

        if (IS_VOID(cell))
            fail (Error_Void_Conditional_Raw());

        return R_FROM_BOOL(IS_TRUTHY(cell)); }

    case REB_DATATYPE: {
        return R_FROM_BOOL(VAL_TYPE_KIND(test) == VAL_TYPE(arg)); }

    case REB_TYPESET: {
        return R_FROM_BOOL(TYPE_CHECK(test, VAL_TYPE(arg))); }

    case REB_BLOCK: {
        RELVAL *item = VAL_ARRAY_AT(test);
        if (IS_END(item)) {
            //
            // !!! If the test is just [], what's that?  People aren't likely
            // to write it literally, but COMPOSE/etc. might make it.
            //
            fail ("No tests found BLOCK! passed to EITHER-TEST.");
        }

        REBSPC *specifier = VAL_SPECIFIER(test);
        for (; NOT_END(item); ++item) {
            const RELVAL *var
                = IS_WORD(item)
                    ? Get_Opt_Var_May_Fail(item, specifier)
                    : item;

            if (IS_DATATYPE(var)) {
                if (VAL_TYPE_KIND(var) == VAL_TYPE(arg))
                    return R_TRUE;
            }
            else if (IS_TYPESET(var)) {
                if (TYPE_CHECK(var, VAL_TYPE(arg)))
                    return R_TRUE;
            }
            else
                fail (Error_Invalid_Type(VAL_TYPE(var)));
        }
        return R_FALSE; }

    default:
        fail (Error_Invalid_Type(VAL_TYPE(arg)));
    }
}


//
//  either-test: native [
//
//  {If argument passes test, return it as-is, otherwise take the branch}
//
//      return: "Input argument if it matched, or branch result"
//          [<opt> any-value!]
//      test "Typeset membership, LOGIC! to test for truth, filter function"
//          [
//              word! path! action! ;-- arity-1 filter function, opt named
//              datatype! typeset! block! ;-- typeset specification forms
//              logic! ;-- tests TO-LOGIC compatibility
//          ]
//      arg [<opt> any-value!]
//      branch "If arity-1 ACTION!, receives the non-matching argument"
//          [block! action!]
//  ]
//
REBNATIVE(either_test)
{
    INCLUDE_PARAMS_OF_EITHER_TEST;

    REB_R r = Either_Test_Core(D_OUT, ARG(test), ARG(arg));
    if (r == R_OUT_IS_THROWN)
        return R_OUT_IS_THROWN;

    if (r == R_TRUE) {
        Move_Value(D_OUT, ARG(arg));
        return R_OUT;
    }

    assert(r == R_FALSE);

    if (Run_Branch_Throws(D_OUT, ARG(arg), ARG(branch)))
        return R_OUT_IS_THROWN;

    return R_OUT;
}


//
//  else: enfix native [
//
//  {If input is not null, return that value, otherwise evaluate the branch}
//
//      return: "Input value if not null, or branch result (possibly null)"
//          [<opt> any-value!]
//      optional "Run branch if this is null (note a VOID! is not null)"
//          [<opt> any-value!]
//      branch "If arity-1 ACTION!, receives null (done for consistency)"
//          [block! action!]
//  ]
//
REBNATIVE(else)
{
    INCLUDE_PARAMS_OF_ELSE; // faster than EITHER-TEST specialized w/`VALUE?`

    if (not IS_NULLED(ARG(optional))) { // note that VOID!s are non-NULL
        Move_Value(D_OUT, ARG(optional));
        return R_OUT;
    }

    if (Run_Branch_Throws(D_OUT, NULLED_CELL, ARG(branch)))
        return R_OUT_IS_THROWN;

    // For ELSE, we do not Voidify_If_Nulled() so that you can write:
    //
    //     if condition [...] else [...] else [...] also [...]
    //
    return R_OUT;
}


//
//  also: enfix native [
//
//  {If input is null, return null, otherwise evaluate the branch}
//
//      return: "null if input is null, or branch result (voided if null)"
//          [<opt> any-value!]
//      optional "Run branch if this is not null (note a VOID! is not null)"
//          [<opt> any-value!]
//      branch "If arity-1 ACTION!, receives value that triggered branch"
//          [block! action!]
//  ]
//
REBNATIVE(also)
{
    INCLUDE_PARAMS_OF_ALSO; // faster than EITHER-TEST specialized w/`NULL?`

    if (IS_NULLED(ARG(optional)))
        return R_NULL;

    if (Run_Branch_Throws(D_OUT, ARG(optional), ARG(branch)))
        return R_OUT_IS_THROWN;

    // For ALSO, we Voidify_If_Nulled() in order to relieve the person writing
    // the branch of worrying about getting out of the way of an ELSE:
    //
    //     switch x [...] also [if y [print "matched!"]] else [...]
    //
    // The concept being that the author means to ELSE the whole expression,
    // e.g. the ALSO and the SWITCH are linked to the "group of things that
    // are associated with success".  Hence the outcome of the ALSO shouldn't
    // be able to accidentally "un-success" it by evaluating to NULL.
    //
    Voidify_If_Nulled(D_OUT);
    return R_OUT;
}


//
//  so: enfix native [
//
//  {"The Lesser ALSO": For non-null input, evaluate and discard branch}
//
//      return: "The same value as input, regardless of if branch runs"
//          [<opt> any-value!]
//      optional "Run branch if this is not null (note a VOID! is not null)"
//          [<opt> any-value!]
//      branch "If arity-1 ACTION!, receives value that triggered branch"
//          [block! action!]
//  ]
//
REBNATIVE(so)
{
    INCLUDE_PARAMS_OF_SO; // `so [...]` faster than `also func [x] [(...) :x]`

    if (IS_NULLED(ARG(optional)))
        return R_NULL;

    if (Run_Branch_Throws(D_OUT, ARG(optional), ARG(branch)))
        return R_OUT_IS_THROWN;

    Move_Value(D_OUT, ARG(optional));
    return R_OUT;
}


//
//  match: native [
//
//  {Check value using tests (match types, TRUE or FALSE, or filter action)}
//
//      return: "Input argument if it matched, otherwise null"
//          [<opt> any-value!]
//      'test "Typeset membership, LOGIC! to test for truth, filter function"
//          [
//              word! path! ;- special "first-arg-stealing" magic
//              lit-word! lit-path! ;-- like EITHER-TEST's WORD! and PATH!
//              datatype! typeset! block! logic! action! ;-- like EITHER-TEST
//          ]
//      :args [any-value! <...>]
//  ]
//
REBNATIVE(match)
//
// This routine soft quotes its `test` argument, and has to be variadic, in
// order to get the special `MATCH PARSE "AAA" [SOME "A"]` -> "AAA" behavior.
// But despite quoting its first argument, it processes it in a way to try
// and mimic EITHER-TEST for compatibility for other cases.
{
    INCLUDE_PARAMS_OF_MATCH;

    REBVAL *test = ARG(test);

    switch (VAL_TYPE(test)) {

    case REB_LIT_WORD:
    case REB_LIT_PATH: {
        if (NOT_VAL_FLAG(test, VALUE_FLAG_UNEVALUATED)) // soft quote eval'd
            fail (Error_Invalid(test)); // disallow `MATCH (QUOTE 'NULL?) ...`

        if (IS_LIT_WORD(test))
            VAL_SET_TYPE_BITS(test, REB_WORD);
        else
            VAL_SET_TYPE_BITS(test, REB_PATH);
        goto either_test; }

    case REB_WORD:
    case REB_PATH: {
        if (NOT_VAL_FLAG(test, VALUE_FLAG_UNEVALUATED)) // soft quote eval'd
            goto either_test; // allow `MATCH ('NULL?) ...`

        REBSTR *opt_label = NULL;
        REBDSP lowest_ordered_dsp = DSP;
        if (Get_If_Word_Or_Path_Throws(
            D_OUT,
            &opt_label,
            test,
            SPECIFIED,
            TRUE // push_refinements
        )){
            return R_OUT_IS_THROWN;
        }

        Move_Value(test, D_OUT);

        if (not IS_ACTION(test)) {
            if (ANY_WORD(test) or ANY_PATH(test))
                fail (Error_Invalid(test)); // disallow `X: 'Y | MATCH X ...`
            goto either_test; // will typecheck the result
        }

        // It was a non-soft quote eval'd word, the kind we want to give the
        // "magical" functionality to.
        //
        // We run the testing function in place in a way that appears "normal"
        // but actually captures its first argument.  That will be MATCH's
        // return value if the filter function returns a truthy result.

        DECLARE_FRAME (f); // REBFRM whose built FRAME! context we will steal

        REBVAL *first_arg;
        if (Make_Invocation_Frame_Throws(
            D_OUT,
            f,
            &first_arg,
            test,
            ARG(args),
            lowest_ordered_dsp
        )){
            return R_OUT_IS_THROWN;
        }

        if (not first_arg)
            fail ("MATCH with a function pattern must take at least 1 arg");

        Move_Value(D_OUT, first_arg); // steal first argument before call

        f->out = D_CELL;

        f->rootvar = CTX_ARCHETYPE(CTX(f->varlist));
        f->param = ACT_FACADE_HEAD(VAL_ACTION(test));
        f->arg = f->rootvar + 1;
        f->special = f->arg;

        f->flags.bits &= ~DO_FLAG_NULLS_UNSPECIALIZED;

        f->refine = ORDINARY_ARG;

        (*PG_Do)(f);

        Drop_Frame_Core(f); // !!! Drop_Frame() asserts f->eval_type as REB_0

        if (THROWN(D_CELL)) {
            Move_Value(D_OUT, D_CELL);
            return R_OUT_IS_THROWN;
        }

        assert(FRM_AT_END(f)); // we started at END_FLAG, can only throw

        if (IS_VOID(D_CELL))
            fail (Error_Void_Conditional_Raw());

        // We still have the first argument from the filter call in D_OUT.

        // MATCH *wants* to pass through the argument on a match, but
        // won't do so if the argument was falsey, as that is misleading.
        // Instead it passes a BAR! back.

        if (IS_TRUTHY(D_CELL)) {
            if (IS_FALSEY(D_OUT))
                return R_BAR;
            return R_OUT;
        }

        return R_NULL; }

    default:
        break;
    }

either_test:;

    // For the "non-magic" cases that are handled by plain EITHER-TEST, call
    // through with the transformed test.  Just take one normal arg via
    // variadic.

    REBVAL *varpar = PAR(args);

    // !!! Hard-quoted arguments don't accept nulls, but we're tweaking the
    // parameter class... make it allow NULL too.
    //
    VAL_TYPESET_BITS(varpar) |= FLAGIT_KIND(REB_MAX_NULLED);
    INIT_VAL_PARAM_CLASS(varpar, PARAM_CLASS_NORMAL); // !!! hack

    REB_R r = Do_Vararg_Op_May_Throw(D_OUT, ARG(args), VARARG_OP_TAKE);

    INIT_VAL_PARAM_CLASS(varpar, PARAM_CLASS_HARD_QUOTE);
    VAL_TYPESET_BITS(varpar) &= ~FLAGIT_KIND(REB_MAX_NULLED);

    if (r == R_OUT_IS_THROWN)
        return R_OUT_IS_THROWN;

    if (r == R_END)
        fail ("Frame hack is written to need argument!");

    assert(r == R_OUT);

    r = Either_Test_Core(D_CELL, test, D_OUT);
    if (r == R_OUT_IS_THROWN)
        return R_OUT_IS_THROWN;

    if (r == R_TRUE) {
        if (IS_FALSEY(D_OUT)) // see above for why false match not passed thru
            return R_BAR;
        return R_OUT;
    }

    assert(r == R_FALSE);
    return R_NULL;
}


//
//  all: native [
//
//  {Short-circuiting variant of AND, using a block of expressions as input}
//
//      return: "Product of last evaluation if all truthy, else null"
//          [<opt> any-value!]
//      block "Block of expressions"
//          [block!]
//  ]
//
REBNATIVE(all)
{
    INCLUDE_PARAMS_OF_ALL;

    DECLARE_FRAME (f);
    Push_Frame(f, ARG(block));

    Init_Nulled(D_OUT); // default return result

    while (FRM_HAS_MORE(f)) {
        if (Do_Next_In_Frame_Throws(D_OUT, f)) {
            Abort_Frame(f);
            return R_OUT_IS_THROWN;
        }

        if (IS_FALSEY(D_OUT)) { // any false/blank/null will trigger failure
            Abort_Frame(f);
            return R_NULL;
        }
    }

    Drop_Frame(f);
    return R_OUT; // successful all when the last D_OUT assignment is truthy
}


//
//  any: native [
//
//  {Short-circuiting version of OR, using a block of expressions as input}
//
//      return: "First truthy evaluative result, or null if all falsey"
//          [<opt> any-value!]
//      block "Block of expressions"
//          [block!]
//  ]
//
REBNATIVE(any)
{
    INCLUDE_PARAMS_OF_ANY;

    DECLARE_FRAME (f);
    Push_Frame(f, ARG(block));

    while (FRM_HAS_MORE(f)) {
        if (Do_Next_In_Frame_Throws(D_OUT, f)) {
            Abort_Frame(f);
            return R_OUT_IS_THROWN;
        }

        if (IS_TRUTHY(D_OUT)) { // successful ANY returns the value
            Abort_Frame(f);
            return R_OUT;
        }
    }

    Drop_Frame(f);
    return R_NULL;
}


//
//  none: native [
//
//  {Short circuiting version of NOR, using a block of expressions as input}
//
//      return: "BAR! if all expressions are falsey, null if any are truthy"
//          [<opt> bar!]
//      block "Block of expressions."
//          [block!]
//  ]
//
REBNATIVE(none)
//
// !!! In order to reduce confusion and accidents in the near term, the
// %mezz-legacy.r renames this to NONE-OF and makes NONE report an error.
{
    INCLUDE_PARAMS_OF_NONE;

    DECLARE_FRAME (f);
    Push_Frame(f, ARG(block));

    while (FRM_HAS_MORE(f)) {
        if (Do_Next_In_Frame_Throws(D_OUT, f)) {
            Abort_Frame(f);
            return R_OUT_IS_THROWN;
        }

        if (IS_TRUTHY(D_OUT)) { // any true results mean failure
            Abort_Frame(f);
            return R_NULL;
        }
    }

    Drop_Frame(f);
    return R_BAR; // "synthetic" truthy that doesn't suggest LOGIC! on failure
}


// Shared code for CASE (which runs BLOCK! clauses as code) and CHOOSE (which
// returns values as-is, e.g. `choose [true [print "hi"]]` => `[print "hi]`
//
static REB_R Case_Choose_Core(
    REBVAL *out,
    REBVAL *cell, // scratch "D_CELL", must be GC safe
    REBVAL *block, // "choices" or "cases", must be GC safe
    REBOOL all,
    REBOOL choose // do not evaluate branches, just "choose" them
){
    DECLARE_FRAME (f);
    Push_Frame(f, block);

    Init_Nulled(out); // default return result

    // With the block argument pushed in the enumerator, that frame slot is
    // available for scratch space in the rest of the routine.

    while (FRM_HAS_MORE(f)) {

        // Perform a DO/NEXT's worth of evaluation on a "condition" to test
        // Will consume any pending "invisibles" (COMMENT, ELIDE, DUMP...)

        if (Do_Next_In_Frame_Throws(cell, f)) {
            Move_Value(out, cell);
            Abort_Frame(f);
            return R_OUT_IS_THROWN;
        }

        // The last condition will "fall out" if there is no branch/choice:
        //
        //     case [1 > 2 [...] 3 > 4 [...] 10 + 20] = 30
        //     choose [1 > 2 (literal group) 3 > 4 <tag> 10 + 20] = 30
        //
        if (FRM_AT_END(f)) {
            Drop_Frame(f);
            Move_Value(out, cell);
            return R_OUT;
        }

        // Regardless of if the condition matches or not, the next value must
        // be valid for the construct.  If it's a CHOOSE operation, it can
        // be any value.  For a CASE be more picky--BLOCK!s only.
        //
        if (not choose and not IS_BLOCK(f->value)) {
            if (IS_ACTION(f->value))
                fail (
                    "ACTION! branches currently not supported in CASE --"
                    " none existed after having the feature for 2 years."
                    " It costs extra to shuffle cells to support passing in"
                    " the condition.  Complain if you have a good reason."
                );
            fail (Error_Invalid_Core(f->value, f->specifier));
        }

        if (IS_CONDITIONAL_FALSE(cell)) { // condition didn't match, skip
            Fetch_Next_In_Frame(f);
            continue;
        }

        // When the condition matches, we must only use the next value
        // literally.  If something like Do_Next_In_Frame_Throws() were
        // called to evaluate the thing-that-became-a-branch, it would also
        // evaluate any ELIDEs after it...which would not be good if /ALL
        // is not set, as it would run code *after* the taken branch.
        //
        if (choose)
            Derelativize(out, f->value, f->specifier); // null not possible
        else {
            if (Do_At_Throws(
                out,
                VAL_ARRAY(f->value),
                VAL_INDEX(f->value),
                f->specifier
            )){
                Abort_Frame(f);
                return R_OUT_IS_THROWN;
            }
            Voidify_If_Nulled(out); // null is reserved for no branch taken
        }

        if (not all) {
            Abort_Frame(f);
            return R_OUT;
        }

        Fetch_Next_In_Frame(f); // keep matching if /ALL
    }

    Drop_Frame(f);
    return R_OUT;
}


//
//  case: native [
//
//  {Evaluates each condition, and when true, evaluates what follows it}
//
//      return: [<opt> any-value!]
//          "Last matched case evaluation, or null if no cases matched"
//      cases [block!]
//          "Block of cases (conditions followed by branches)"
//      /all
//          "Evaluate all cases (do not stop at first logically true case)"
//  ]
//
REBNATIVE(case)
{
    INCLUDE_PARAMS_OF_CASE;

    const REBOOL choose = FALSE;
    return Case_Choose_Core(D_OUT, D_CELL, ARG(cases), REF(all), choose);
}


//
//  choose: native [
//
//  {Evaluates each condition, and gives back the value that follows it}
//
//      return: [<opt> any-value!]
//          "Last matched choice value, or void if no choices matched"
//      choices [block!]
//          "Evaluate all choices (do not stop at first TRUTHY? choice)"
//      /all
//          "Return the value for the last matched choice (instead of first)"
//  ]
//
REBNATIVE(choose)
{
    INCLUDE_PARAMS_OF_CHOOSE;

    // The choose can't be run backwards, only forwards.  So implementation
    // means that "/LAST" really can only be done as an /ALL, there's no way
    // to go backwards in the block and get a Rebol-coherent answer.  Calling
    // it /ALL instead of /LAST helps reinforce that *all the conditions*
    // will be evaluated.
    //
    const REBOOL all = REF(all);

    const REBOOL choose = true;
    return Case_Choose_Core(D_OUT, D_CELL, ARG(choices), all, choose);
}


//
//  switch: native [
//
//  {Selects a choice and evaluates the block that follows it.}
//
//      return: "Last case evaluation, or null if no cases matched"
//          [<opt> any-value!]
//      value "Target value"
//          [<opt> any-value!]
//      cases "Block of cases (comparison lists followed by block branches)"
//          [block!]
//      /all "Evaluate all matches (not just first one)"
//      ; !!! /STRICT may have a different name
//      ; https://forum.rebol.info/t/349
//      /strict "Use STRICT-EQUAL? when comparing cases instead of EQUAL?"
//      ; !!! Is /QUOTE truly needed?
//      /quote "Do not evaluate comparison values"
//      ; !!! Needed in spec for ADAPT to override in shim
//      /default "Deprecated: use fallout feature or ELSE, UNLESS, etc."
//      default-branch [block!]
//  ]
//
REBNATIVE(switch)
{
    INCLUDE_PARAMS_OF_SWITCH;

    if (REF(default))
        fail (
            "SWITCH/DEFAULT is no longer supported by the core.  Use the"
            " fallout feature, or ELSE/UNLESS/!!/etc. based on null result:"
            " https://forum.rebol.info/t/312"
        );
    UNUSED(ARG(default_branch));

    DECLARE_FRAME (f);
    Push_Frame(f, ARG(cases));

    REBVAL *value = ARG(value);

    if (IS_BLOCK(value) and GET_VAL_FLAG(value, VALUE_FLAG_UNEVALUATED))
        fail (Error_Block_Switch_Raw(value)); // `switch [x] [...]` safeguard

    Init_Nulled(D_OUT); // used for "fallout"

    while (FRM_HAS_MORE(f)) {
        //
        // If a branch is seen at this point, it doesn't correspond to any
        // condition to match.  If no more tests are run, let it suppress the
        // feature of the last value "falling out" the bottom of the switch
        //
        if (IS_BLOCK(f->value)) {
            Init_Nulled(D_OUT);
            Fetch_Next_In_Frame(f);
            continue;
        }

        if (IS_ACTION(f->value)) {
            //
            // It's a literal ACTION!, e.g. one composed in the block:
            //
            //    switch :some-func compose [
            //        :append [print "not this case... this is fine"]
            //        :insert (:branch) ;-- it's this situation
            //    ]
            //
        action_not_supported:
            fail (
                "ACTION! branches currently not supported in SWITCH --"
                " none existed after having the feature for 2 years."
                " Complain if you found a good use for it."
            );
        }

        if (REF(quote))
            Quote_Next_In_Frame(D_OUT, f);
        else {
            if (Do_Next_In_Frame_Throws(D_OUT, f)) {
                Abort_Frame(f);
                return R_OUT_IS_THROWN;
            }
        }

        // It's okay that we are letting the comparison change `value`
        // here, because equality is supposed to be transitive.  So if it
        // changes 0.01 to 1% in order to compare it, anything 0.01 would
        // have compared equal to so will 1%.  (That's the idea, anyway,
        // required for `a = b` and `b = c` to properly imply `a = c`.)
        //
        // !!! This means fallout can be modified from its intent.  Rather
        // than copy here, this is a reminder to review the mechanism by
        // which equality is determined--and why it has to mutate.
        //
        // !!! A branch composed into the switch cases block may want to see
        // the un-mutated condition value, in which case this should not
        // be changing D_CELL

        if (!Compare_Modify_Values(ARG(value), D_OUT, REF(strict) ? 1 : 0))
            continue;

        // Skip ahead to try and find a block, to treat as code for the match

        while (true) {
            if (FRM_AT_END(f)) {
                Drop_Frame(f);
                return R_OUT; // last test "falls out", might be void
            }
            if (IS_BLOCK(f->value))
                break;
            if (IS_ACTION(f->value))
                goto action_not_supported; // literal action
            Fetch_Next_In_Frame(f);
        }

        if (Do_At_Throws( // it's a match, so run the BLOCK!
            D_OUT,
            VAL_ARRAY(f->value),
            VAL_INDEX(f->value),
            f->specifier
        )){
            Abort_Frame(f);
            return R_OUT_IS_THROWN;
        }

        Voidify_If_Nulled(D_OUT); // null is reserved for no branch run

        if (not REF(all)) {
            Abort_Frame(f);
            return R_OUT;
        }

        Fetch_Next_In_Frame(f); // keep matching if /ALL
    }

    Drop_Frame(f);
    return R_OUT; // last test "falls out" or last match if /ALL, may be void
}


//
//  catch: native [
//
//  {Catches a throw from a block and returns its value.}
//
//      return: [<opt> any-value!]
//      block "Block to evaluate"
//          [block!]
//      /name "Catches a named throw" ;-- should it be called /named ?
//      names "Names to catch (single name if not block)"
//          [block! word! action! object!]
//      /quit "Special catch for QUIT native"
//      /any "Catch all throws except QUIT (can be used with /QUIT)"
//      /with "Handle thrown case with code"
//      handler "If action, the spec matches [value name]"
//          [block! action!]
//  ]
//
REBNATIVE(catch)
//
// There's a refinement for catching quits, and CATCH/ANY will not alone catch
// it (you have to CATCH/ANY/QUIT).  Currently the label for quitting is the
// NATIVE! function value for QUIT.
{
    INCLUDE_PARAMS_OF_CATCH;

    // /ANY would override /NAME, so point out the potential confusion
    //
    if (REF(any) and REF(name))
        fail (Error_Bad_Refines_Raw());

    if (Do_Any_Array_At_Throws(D_OUT, ARG(block))) {
        if (REF(any) and not (
            IS_ACTION(D_OUT)
            and VAL_ACT_DISPATCHER(D_OUT) == &N_quit
        )){
            goto was_caught;
        }

        if (REF(quit) and (
            IS_ACTION(D_OUT)
            and VAL_ACT_DISPATCHER(D_OUT) == &N_quit
        )){
            goto was_caught;
        }

        if (REF(name)) {
            //
            // We use equal? by way of Compare_Modify_Values, and re-use the
            // refinement slots for the mutable space

            REBVAL *temp1 = ARG(quit);
            REBVAL *temp2 = ARG(any);

            // !!! The reason we're copying isn't so the VALUE_FLAG_THROWN bit
            // won't confuse the equality comparison...but would it have?

            if (IS_BLOCK(ARG(names))) {
                //
                // Test all the words in the block for a match to catch

                RELVAL *candidate = VAL_ARRAY_AT(ARG(names));
                for (; NOT_END(candidate); candidate++) {
                    //
                    // !!! Should we test a typeset for illegal name types?
                    //
                    if (IS_BLOCK(candidate))
                        fail (Error_Invalid(ARG(names)));

                    Derelativize(temp1, candidate, VAL_SPECIFIER(ARG(names)));
                    Move_Value(temp2, D_OUT);

                    // Return the THROW/NAME's arg if the names match
                    // !!! 0 means equal?, but strict-equal? might be better
                    //
                    if (Compare_Modify_Values(temp1, temp2, 0))
                        goto was_caught;
                }
            }
            else {
                Move_Value(temp1, ARG(names));
                Move_Value(temp2, D_OUT);

                // Return the THROW/NAME's arg if the names match
                // !!! 0 means equal?, but strict-equal? might be better
                //
                if (Compare_Modify_Values(temp1, temp2, 0))
                    goto was_caught;
            }
        }
        else {
            // Return THROW's arg only if it did not have a /NAME supplied
            //
            if (IS_BLANK(D_OUT))
                goto was_caught;
        }

        // Throw name is in D_OUT, thrown value is held task local
        //
        return R_OUT_IS_THROWN;
    }

    return R_OUT;

was_caught:
    if (REF(with)) {
        REBVAL *handler = ARG(handler);

        // We again re-use the refinement slots, but this time as mutable
        // space protected from GC for the handler's arguments
        //
        REBVAL *thrown_arg = ARG(any);
        REBVAL *thrown_name = ARG(quit);

        CATCH_THROWN(thrown_arg, D_OUT);
        Move_Value(thrown_name, D_OUT); // THROWN bit cleared by CATCH_THROWN

        if (IS_BLOCK(handler)) {
            //
            // There's no way to pass args to a block (so just DO it)
            //
            if (Do_Any_Array_At_Throws(D_OUT, ARG(handler)))
                return R_OUT_IS_THROWN;

            return R_OUT;
        }
        else if (IS_ACTION(handler)) {
            //
            // This calls the function but only does a DO/NEXT.  Hence the
            // function might be arity 0, arity 1, or arity 2.  If it has
            // greater arity it will process more arguments.
            //
            if (Apply_Only_Throws(
                D_OUT,
                FALSE, // do not alert if handler doesn't consume all args
                handler,
                NULLIZE(thrown_arg), // convert nulled cells to NULL for API
                NULLIZE(thrown_name), // convert nulled cells to NULL for API
                END
            )){
                return R_OUT_IS_THROWN;
            }

            return R_OUT;
        }
    }

    // If no handler, just return the caught thing
    //
    CATCH_THROWN(D_OUT, D_OUT);

    return R_OUT;
}


//
//  throw: native [
//
//  "Throws control back to a previous catch."
//
//      value "Value returned from catch"
//          [<opt> any-value!]
//      /name "Throws to a named catch"
//      name-value [word! action! object!]
//  ]
//
REBNATIVE(throw)
//
// Choices are currently limited for what one can use as a "name" of a THROW.
// Note blocks as names would conflict with the `name_list` feature in CATCH.
//
// !!! Should parameters be /NAMED and NAME ?
{
    INCLUDE_PARAMS_OF_THROW;

    REBVAL *value = ARG(value);

    if (IS_ERROR(value)) {
        //
        // We raise an alert from within the implementation of throw for
        // trying to use it to trigger errors, because if THROW just didn't
        // take errors in the spec it wouldn't guide what *to* use.
        //
        fail (Error_Use_Fail_For_Error_Raw(value));

        // Note: Caller can put the ERROR! in a block or use some other
        // such trick if it wants to actually throw an error.
        // (Better than complicating via THROW/ERROR-IS-INTENTIONAL!)
    }

    if (REF(name))
        Move_Value(D_OUT, ARG(name_value));
    else {
        // Blank values serve as representative of THROWN() means "no name"
        //
        Init_Blank(D_OUT);
    }

    CONVERT_NAME_TO_THROWN(D_OUT, value);
    return R_OUT_IS_THROWN;
}
