//
//  File: %n-function.c
//  Summary: "Generator for an ACTION! whose body is a block of user code"
//  Section: natives
//  Project: "Ren-C Language Interpreter and Run-time Environment"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012-2021 Ren-C Open Source Contributors
// Copyright 2012 REBOL Technologies
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Lesser GPL, Version 3.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.gnu.org/licenses/lgpl-3.0.html
//
//=////////////////////////////////////////////////////////////////////////=//
//
// FUNC is a common means for creating an action from a BLOCK! of code, with
// another block serving as the "spec" for parameters and HELP:
//
//     >> print-sum-twice: func [
//            {Prints the sum of two integers, and return the sum}
//            return: "The sum" [integer!]
//            x "First Value" [integer!]
//            y "Second Value" [integer!]
//            <local> sum
//        ][
//            sum: x + y
//            repeat 2 [print ["The sum is" sum]]
//            return sum
//        ]
//
//     >> print-sum-twice 10 20
//     The sum is 30
//     The sum is 30
//
// Ren-C brings new abilities not present in historical Rebol:
//
// * Return-type checking via `return: [...]` in the spec
//
// * Definitional RETURN, so that each FUNC has a local definition of its
//   own version of return specially bound to its invocation.
//
// * Specific binding of arguments, so that each instance of a recursion
//   can discern WORD!s from each recursion.  (In R3-Alpha, this was only
//   possible using CLOSURE which made a costly deep copy of the function's
//   body on every invocation.  Ren-C's method does not require a copy.)
//
// * Invisible functions (return: [nihil?]) that vanish completely,
//   leaving whatever result was in the evaluation previous to the function
//   call as-is.
//
// * Refinements-as-their-own-arguments--which streamlines the evaluator,
//   saves memory, simplifies naming, and simplifies the FRAME! mechanics.
//
//=//// NOTES /////////////////////////////////////////////////////////////=//
//
// * R3-Alpha defined FUNC in terms of MAKE ACTION! on a block.  There was
//   no particular advantage to having an entry point to making functions
//   from a spec and body that put them both in the same block, so FUNC
//   serves as a more logical native entry point for that functionality.
//
// * While FUNC is intended to be an optimized native due to its commonality,
//   the belief is still that it should be possible to build an equivalent
//   (albeit slower) version in usermode out of other primitives.  The current
//   plan is that those primitives would be RUNS of a FRAME!, and being able
//   to ADAPT a block of code into that frame.  This makes ADAPT the more
//   foundational operation for fusing interfaces with block bodies.
//

#include "sys-core.h"


//
//  Func_Dispatcher: C
//
// Puts a definitional return ACTION! in the RETURN slot of the frame, and
// runs the body block associated with this function.
//
//=////////////////////////////////////////////////////////////////////////=//
//
// 1. FUNC(TION) evaluates into the SPARE cell, because the body result is
//    never used as a return value.  Only RETURN can give non-"trash".
//
// 2. If no RETURN statement is given, the result is trash, and typechecking
//    is performed to make sure trash was a legitimate return.  This has a
//    little bit of a negative side in that if someone is to hook the RETURN
//    function, it will not be called in these "fallout" cases.  It's deemed
//    too ugly to slip in a "hidden" call to RETURN for this case, and too
//    much of a hassle to force people to put RETURN ~ or RETURN at the end.
//    So this is the compromise chosen...at the moment.
//
Bounce Func_Dispatcher(Level* const L)
{
    USE_LEVEL_SHORTHANDS (L);

    enum {
        ST_FUNC_INITIAL_ENTRY = STATE_0,
        ST_FUNC_BODY_EXECUTING
    };

    switch (STATE) {
      case ST_FUNC_INITIAL_ENTRY: goto initial_entry;
      case ST_FUNC_BODY_EXECUTING: goto body_finished_without_returning;
      default: assert(false);
    }

  initial_entry: {  //////////////////////////////////////////////////////////

    Details* details = Phase_Details(PHASE);
    Cell* body = Array_At(details, IDX_DETAILS_1);  // code to run
    assert(Is_Block(body) and Is_Relative(body) and VAL_INDEX(body) == 0);

    assert(ACT_HAS_RETURN(PHASE));  // all FUNC have RETURN
    assert(KEY_SYM(ACT_KEYS_HEAD(PHASE)) == SYM_RETURN);

    REBVAL *cell = Level_Arg(L, 1);
    assert(Is_Nulled(cell));
    Force_Level_Varlist_Managed(L);
    Init_Action(
        cell,
        ACT_IDENTITY(VAL_ACTION(Lib(DEFINITIONAL_RETURN))),
        Canon(RETURN),  // relabel (the RETURN in lib is a dummy action)
        cast(Context*, L->varlist)  // so RETURN knows where to return from
    );

    STATE = ST_FUNC_BODY_EXECUTING;

    assert(Is_Fresh(SPARE));
    return CONTINUE_CORE(
        SPARE,  // body evaluative result discarded [1]
        LEVEL_MASK_NONE,  // no DISPATCHER_CATCHES, RETURN is responsible
        SPC(L->varlist), body
    );

} body_finished_without_returning: {  ////////////////////////////////////////

    Init_Trash(OUT);  // trash, regardless of body result [2]

    if (not Typecheck_Coerce_Return(L, OUT))
        fail ("End of function without a RETURN, but ~ not in RETURN: spec");

    return Proxy_Multi_Returns(L);
}}


//
//  Make_Interpreted_Action_May_Fail: C
//
// This digests the spec block into a `paramlist` for parameter descriptions,
// along with an associated `keylist` of the names of the parameters and
// various locals.  A separate object that uses the same keylist is made
// which maps the parameters to any descriptions that were in the spec.
//
// Due to the fact that the typesets in paramlists are "lossy" of information
// in the source, another object is currently created as well that maps the
// parameters to the BLOCK! of type information as it appears in the source.
// Attempts are being made to close the gap between that and the paramlist, so
// that separate arrays aren't needed for this closely related information:
//
// https://forum.rebol.info/t/1459
//
// The C function dispatcher that is used for the resulting ACTION! varies.
// For instance, if the body is empty then it picks a dispatcher that does
// not bother running the code.  And if there's no return type specified,
// a dispatcher that doesn't check the type is used.
//
// There is also a "definitional return" MKF_RETURN option used by FUNC, so
// the body will introduce a RETURN specific to each action invocation, thus
// acting more like:
//
//     return: lambda
//         [{Returns a value from a function.} ^value [any-atom?]]
//         [unwind/with (binding of 'return) unmeta value]
//     ]
//     (body goes here)
//
// This pattern addresses "Definitional Return" in a way that does not need to
// build in RETURN as a language keyword in any specific form (in the sense
// that functions do not itself require it).  See the LAMBDA generator for
// an example...where UNWIND can be used to exit frames if you want to build
// something return-like.
//
// FUNC optimizes by not internally building or executing the equivalent body,
// but giving it back from BODY-OF.  This gives FUNC the edge to pretend to
// add containing code and simulate its effects, while really only holding
// onto the body the caller provided.
//
//=////////////////////////////////////////////////////////////////////////=//
//
// 1. At one time there were many optimized dispatchers for cases like
//    `func [...] []` which would not bother running empty blocks, and which
//    did not write into a temporary cell and then copy over the result in
//    a later phase.  The introduction of LAMBDA as an alternative generator
//    made these optimizations give diminishing returns, so they were all
//    eliminated (though they set useful precedent for varying dispatchers).
//
Phase* Make_Interpreted_Action_May_Fail(
    const REBVAL *spec,
    const REBVAL *body,
    Flags mkf_flags,  // MKF_RETURN, etc.
    Dispatcher* dispatcher,
    REBLEN details_capacity
){
    assert(Is_Block(spec) and Is_Block(body));
    assert(details_capacity >= 1);  // relativized body put in details[0]

    Context* meta;
    Array* paramlist = Make_Paramlist_Managed_May_Fail(
        &meta,
        spec,
        &mkf_flags
    );

    Phase* a = Make_Action(
        paramlist,
        nullptr,  // no partials
        dispatcher,
        details_capacity  // we fill in details[0], caller fills any extra
    );

    assert(ACT_ADJUNCT(a) == nullptr);
    mutable_ACT_ADJUNCT(a) = meta;

    Array* copy = Copy_And_Bind_Relative_Deep_Managed(
        body,  // new copy has locals bound relatively to the new action
        a,
        VAR_VISIBILITY_ALL // we created exemplar, see all!
    );

    // Favor the spec first, then the body, for file and line information.
    //
    if (Get_Array_Flag(Cell_Array(spec), HAS_FILE_LINE_UNMASKED)) {
        LINK(Filename, copy) = LINK(Filename, Cell_Array(spec));
        copy->misc.line = Cell_Array(spec)->misc.line;
        Set_Array_Flag(copy, HAS_FILE_LINE_UNMASKED);
    }
    else if (
        Get_Array_Flag(Cell_Array(body), HAS_FILE_LINE_UNMASKED)
    ){
        LINK(Filename, copy) = LINK(Filename, Cell_Array(body));
        copy->misc.line = Cell_Array(body)->misc.line;
        Set_Array_Flag(copy, HAS_FILE_LINE_UNMASKED);
    }
    else {
        // Ideally all source series should have a file and line numbering
        // At the moment, if a function is created in the body of another
        // function it doesn't work...trying to fix that.
    }

    // Save the relativized body in the action's details block.  Since it is
    // a Cell* and not a REBVAL*, the dispatcher must combine it with a
    // running frame instance (the Level* received by the dispatcher) before
    // executing the interpreted code.
    //
    Details* details = Phase_Details(a);
    Cell* rebound = Init_Relative_Block(
        Array_At(details, IDX_NATIVE_BODY),
        a,
        copy
    );

    // Capture the mutability flag that was in effect when this action was
    // created.  This allows the following to work:
    //
    //    >> do mutable [f: function [] [b: [1 2 3] clear b]]
    //    >> f
    //    == []
    //
    // So even though the invocation is outside the mutable section, we have
    // a memory that it was created under those rules.  (It's better to do
    // this based on the frame in effect than by looking at the CONST flag of
    // the incoming body block, because otherwise ordinary Ren-C functions
    // whose bodies were created from dynamic code would have mutable bodies
    // by default--which is not a desirable consequence from merely building
    // the body dynamically.)
    //
    // Note: besides the general concerns about mutability-by-default, when
    // functions are allowed to modify their bodies with words relative to
    // their frame, the words would refer to that specific recursion...and not
    // get picked up by other recursions that see the common structure.  This
    // means compatibility would be with the behavior of R3-Alpha CLOSURE,
    // not with R3-Alpha FUNCTION.
    //
    if (Get_Cell_Flag(body, CONST))
        Set_Cell_Flag(rebound, CONST);  // Inherit_Const() would need REBVAL*

    return a;
}


//
//  func*: native [
//
//  "Defines an ACTION! with given spec and body"
//
//      return: [action?]
//      spec "Help string (opt) followed by arg words (and opt type + string)"
//          [block!]
//      body "Code implementing the function--use RETURN to yield a result"
//          [block!]
//  ]
//
DECLARE_NATIVE(func_p)
{
    INCLUDE_PARAMS_OF_FUNC_P;

    REBVAL *spec = ARG(spec);
    REBVAL *body = ARG(body);

    Phase* func = Make_Interpreted_Action_May_Fail(
        spec,
        body,
        MKF_RETURN | MKF_KEYWORDS,
        &Func_Dispatcher,
        1 + IDX_DETAILS_1  // archetype and one array slot (will be filled)
    );

    return Init_Action(OUT, func, ANONYMOUS, UNBOUND);
}


//
//  endable?: native [
//
//  {Tell whether a parameter is registered as <end> or not}
//
//      return: [logic?]
//      parameter [word!]
//  ]
//
DECLARE_NATIVE(endable_q)
//
// !!! The general mechanics by which parameter properties are extracted have
// not been designed.  This extraction feature was added to support making
// semi-"variadic" combinators in UPARSE, but better is needed.
{
    INCLUDE_PARAMS_OF_ENDABLE_Q;

    REBVAL *v = ARG(parameter);

    if (not Did_Get_Binding_Of(SPARE, v))
        fail (PARAM(parameter));

    if (not Is_Frame(SPARE))
        fail ("ENDABLE? requires a WORD! bound into a FRAME! at present");

    Context* ctx = VAL_CONTEXT(SPARE);
    Action* act = CTX_FRAME_PHASE(ctx);

    Param* param = ACT_PARAM(act, VAL_WORD_INDEX(v));
    bool endable = Get_Parameter_Flag(param, ENDABLE);

    return Init_Logic(OUT, endable);
}


//
//  skippable?: native [
//
//  {Tell whether a parameter is registered as <skip> or not}
//
//      return: [logic?]
//      parameter [word!]
//  ]
//
DECLARE_NATIVE(skippable_q)
//
// !!! The general mechanics by which parameter properties are extracted have
// not been designed.  This extraction feature was added to support making
// combinators that could <skip> arguments in UPARSE, but better is needed.
{
    INCLUDE_PARAMS_OF_SKIPPABLE_Q;

    REBVAL *v = ARG(parameter);

    if (not Did_Get_Binding_Of(SPARE, v))
        fail (PARAM(parameter));

    if (not Is_Frame(SPARE))
        fail ("SKIPPABLE? requires a WORD! bound into a FRAME! at present");

    Context* ctx = VAL_CONTEXT(SPARE);
    Action* act = CTX_FRAME_PHASE(ctx);

    Param* param = ACT_PARAM(act, VAL_WORD_INDEX(v));
    bool skippable = Get_Parameter_Flag(param, SKIPPABLE);

    return Init_Logic(OUT, skippable);
}


//
//  Init_Thrown_Unwind_Value: C
//
// This routine generates a thrown signal that can be used to indicate a
// desire to jump to a particular level in the stack with a return value.
// It is used in the implementation of the UNWIND native.
//
// See notes is %sys-frame.h about how there is no actual REB_THROWN type.
//
Bounce Init_Thrown_Unwind_Value(
    Level* level_,
    const REBVAL *seek, // FRAME!, ACTION! (or INTEGER! relative to frame)
    Atom(const*) value,
    Level* target // required if level is INTEGER! or ACTION!
) {
    DECLARE_STABLE (label);
    Copy_Cell(label, Lib(UNWIND));

    if (Is_Frame(seek) and Is_Frame_On_Stack(VAL_CONTEXT(seek))) {
        g_ts.unwind_level = CTX_LEVEL_IF_ON_STACK(VAL_CONTEXT(seek));
    }
    else if (Is_Frame(seek)) {
        Level* L = target->prior;
        for (; true; L = L->prior) {
            if (L == BOTTOM_LEVEL)
                fail (Error_Invalid_Exit_Raw());

            if (not Is_Action_Level(L))
                continue; // only exit functions

            if (Is_Level_Fulfilling(L))
                continue; // not ready to exit

            if (VAL_ACTION(seek) == L->u.action.original) {
                g_ts.unwind_level = L;
                break;
            }
        }
    }
    else {
        assert(Is_Integer(seek));

        REBLEN count = VAL_INT32(seek);
        if (count <= 0)
            fail (Error_Invalid_Exit_Raw());

        Level* L = target->prior;
        for (; true; L = L->prior) {
            if (L == BOTTOM_LEVEL)
                fail (Error_Invalid_Exit_Raw());

            if (not Is_Action_Level(L))
                continue; // only exit functions

            if (Is_Level_Fulfilling(L))
                continue; // not ready to exit

            --count;
            if (count == 0) {
                g_ts.unwind_level = L;
                break;
            }
        }
    }

    return Init_Thrown_With_Label(level_, value, label);
}


//
//  unwind: native [
//
//  {Jump up the stack to return from a specific frame or call.}
//
//      return: []  ; "divergent"
//      level "Frame or index to exit from"
//          [frame! integer!]
//      ^result "Result for enclosing state"
//          [any-atom?]
//  ]
//
DECLARE_NATIVE(unwind)
//
// UNWIND is implemented via a throw that bubbles through the stack.  Using
// UNWIND's action REBVAL with a target `binding` field is the protocol
// understood by Eval_Core to catch a throw itself.
//
// !!! Allowing to pass an INTEGER! to jump from a function based on its
// BACKTRACE number is a bit low-level, and perhaps should be restricted to
// a debugging mode (though it is a useful tool in "code golf").
//
// !!! This might be a little more natural if the label of the throw was a
// FRAME! value.  But that also would mean throws named by frames couldn't be
// taken advantage by the user for other features, while this only takes one
// function away.  (Or, perhaps Isotope frames could be used?)
{
    INCLUDE_PARAMS_OF_UNWIND;

    REBVAL *level = ARG(level);

    Copy_Cell(SPARE, ARG(result));  // SPARE can hold unstable isotopes
    Meta_Unquotify_Undecayed(SPARE);

    return Init_Thrown_Unwind_Value(LEVEL, level, SPARE, level_);
}


//
//  Typecheck_Coerce_Return: C
//
bool Typecheck_Coerce_Return(
    Level* L,
    Atom(*) atom  // coercion needs mutability
){
    if (Is_Raised(atom))
        return true;  // For now, all functions return definitional errors

    // Typeset bits for locals in frames are usually ignored, but the RETURN:
    // local uses them for the return types of a function.
    //
    Phase* phase = Level_Phase(L);
    const Param* param = ACT_PARAMS_HEAD(phase);
    assert(KEY_SYM(ACT_KEYS_HEAD(phase)) == SYM_RETURN);

    if (Get_Parameter_Flag(param, TRASH_DEFINITELY_OK) and Is_Trash(atom))
        return true;  // common case, make fast

    if (Get_Parameter_Flag(param, NIHIL_DEFINITELY_OK) and Is_Nihil(atom))
        return true;  // kind of common... necessary?

    if (Typecheck_Coerce_Argument(param, atom))
        return true;

    if (Is_Nihil(atom)) {  // RETURN NIHIL
        //
        // !!! Treating a return of NIHIL as a return of trash helps some
        // scenarios, for instance piping UPARSE combinators which do not
        // want to propagate pure invisibility.  The idea should be reviewed
        // to see if VOID makes more sense...but start with a more "ornery"
        // value to see how it shapes up.
        //
        Init_Trash(atom);
    }

    return Typecheck_Coerce_Argument(param, atom);
}


//
//  definitional-return: native [
//
//  {RETURN, giving a result to the caller}
//
//      return: []  ; "divergent"
//      ^value [any-atom?]
//      /only "Don't proxy output variables, return argument without typecheck"
//  ]
//
DECLARE_NATIVE(definitional_return)
//
// Returns in Ren-C are functions that are aware of the function they return
// to.  So the dispatchers for functions that provide return e.g. FUNC will
// actually use an instance of this native, and poke a binding into it to
// identify the action.
//
// This means the RETURN that is in LIB is actually just a dummy function
// which you will bind to and run if there is no definitional return in effect.
//
// 1. The cached name for values holding this native is set to RETURN by the
//    dispatchers that use it, which might seem confusing debugging this.
//
// 2. Check type NOW instead of waiting and having the dispatcher check it.
//    Reasoning is that that lets the error indicate the callsite, e.g. the
//    point where `return badly-typed-value` happened.
//
//    !!! In the userspace formulation of this abstraction, it indicates
//    it's not RETURN's type signature that is constrained, as if it were
//    then RETURN would be implicated in the error.  Instead, RETURN must
//    take [any-atom?] as its argument, and then report the error itself...
//    implicating the frame (in a way parallel to this native).
{
    INCLUDE_PARAMS_OF_DEFINITIONAL_RETURN;  // cached name usually RETURN [1]

    Atom(*) atom = Copy_Cell(SPARE, ARG(value));  // SPARE for unstable atoms
    Meta_Unquotify_Undecayed(atom);

    Level* return_level = LEVEL;  // Level of this RETURN call

    Context* binding = Level_Binding(return_level);  // see definition
    if (not binding)
        fail (Error_Unbound_Archetype_Raw());

    Level* target_level = CTX_LEVEL_MAY_FAIL(binding);
    if (
        not REF(only)
        and not Typecheck_Coerce_Return(target_level, atom)  // check NOW! [2]
    ){
        fail (Error_Bad_Return_Type(target_level, atom));
    }

    DECLARE_STABLE (label);
    Copy_Cell(label, Lib(UNWIND)); // see Make_Thrown_Unwind_Value
    g_ts.unwind_level = target_level;

    if (not Is_Raised(atom) and not REF(only))
        Proxy_Multi_Returns_Core(target_level, atom);

    return Init_Thrown_With_Label(LEVEL, atom, label);
}


//
//  inherit-adjunct: native [
//
//  {Copy help information from the original function to the derived function}
//
//      return: "Same as derived (assists in efficient chaining)"
//          [action?]
//      derived [<unrun> frame!]
//      original [<unrun> frame!]
//      /augment "Additional spec information to scan"
//          [block!]
//  ]
//
DECLARE_NATIVE(inherit_adjunct)
{
    INCLUDE_PARAMS_OF_INHERIT_ADJUNCT;

    REBVAL *derived = ARG(derived);
    QUOTE_BYTE(derived) = ISOTOPE_0;  // ensure return is isotope

    const REBVAL *original = ARG(original);

    if (
        not Is_Frame_Details(original)
        or not Is_Frame_Details(derived)
    ){
        return COPY(ARG(derived));  // !!! temporary (?) weakness
    }

    UNUSED(ARG(augment));  // !!! not yet implemented

    Context* a1 = ACT_ADJUNCT(VAL_ACTION(original));
    if (not a1)  // nothing to copy
        return COPY(ARG(derived));

    // Often the derived function won't have its own adjunct information yet.
    // But if it was created via an AUGMENT, it will have some...only the notes
    // and types for the added parameters, the others will be NULL.
    //
    Context* a2 = ACT_ADJUNCT(VAL_ACTION(derived));
    if (not a2) {  // doesn't have its own information
        a2 = Copy_Context_Shallow_Managed(VAL_CONTEXT(Root_Action_Adjunct));
        mutable_ACT_ADJUNCT(VAL_ACTION(derived)) = a2;
    }

    // By default, inherit description (though ideally, they should have
    // changed it to explain why it's different).
    //
    Option(Value(*)) description2 = Select_Symbol_In_Context(
        CTX_ARCHETYPE(a2),
        Canon(DESCRIPTION)
    );
    if (description2 and Is_Nulled(unwrap(description2))) {
        Option(Value(*)) description1 = Select_Symbol_In_Context(
            CTX_ARCHETYPE(a1),
            Canon(DESCRIPTION)
        );
        if (description1)
            Copy_Cell(unwrap(description2), unwrap(description1));
    }

    REBLEN which = 0;
    Option(SymId) syms[] = {SYM_PARAMETER_NOTES, SYM_PARAMETER_TYPES, SYM_0};

    for (; syms[which] != 0; ++which) {
        Value(*) val1 = try_unwrap(Select_Symbol_In_Context(
            CTX_ARCHETYPE(a1),
            Canon_Symbol(unwrap(syms[which]))
        ));
        if (not val1 or Is_Nulled(val1) or Is_Trash(val1))
            continue;  // nothing to inherit from
        if (not Any_Context(val1))
            fail ("Expected context in original meta information");

        Context* ctx1 = VAL_CONTEXT(val1);

        Value(*) val2 = try_unwrap(Select_Symbol_In_Context(
            CTX_ARCHETYPE(a2),
            Canon_Symbol(unwrap(syms[which]))
        ));
        if (not val2)
            continue;

        Context* ctx2;
        if (Is_Nulled(val2) or Is_Trash(val2)) {
            ctx2 = Make_Context_For_Action(
                derived,  // the action
                TOP_INDEX,  // would weave in refinements pushed (none apply)
                nullptr  // !!! review, use fast map from names to indices
            );
            Init_Frame(val2, ctx2, ANONYMOUS);
        }
        else if (Any_Context(val2)) {  // already had context (e.g. augment)
            ctx2 = VAL_CONTEXT(val2);
        }
        else
            fail ("Expected context in derived meta information");

        EVARS e;
        Init_Evars(&e, val2);

        while (Did_Advance_Evars(&e)) {
            if (not Is_Trash(e.var) and not Is_Nulled(e.var))
                continue;  // already set to something

            Option(Value(*)) slot = Select_Symbol_In_Context(
                CTX_ARCHETYPE(ctx1),
                KEY_SYMBOL(e.key)
            );
            if (slot)
                Copy_Cell(e.var, unwrap(slot));
            else
                Init_Nulled(e.var);  // don't want to leave as `~` isotope
        }

        Shutdown_Evars(&e);
    }

    return COPY(ARG(derived));
}
