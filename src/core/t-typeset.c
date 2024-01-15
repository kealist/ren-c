//
//  File: %t-typeset.c
//  Summary: "typeset datatype"
//  Section: datatypes
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Ren-C Open Source Contributors
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

#include "sys-core.h"


//
//  CT_Parameter: C
//
REBINT CT_Parameter(NoQuote(const Cell*) a, NoQuote(const Cell*) b, bool strict)
{
    UNUSED(strict);

    assert(Cell_Heart(a) == REB_PARAMETER);
    assert(Cell_Heart(b) == REB_PARAMETER);

    if (Cell_Parameter_Spec(a) != Cell_Parameter_Spec(b))
        return Cell_Parameter_Spec(a) > Cell_Parameter_Spec(b) ? 1 : -1;

    if (Cell_Parameter_String(a) != Cell_Parameter_String(b))
        return Cell_Parameter_String(a) > Cell_Parameter_String(b) ? 1 : -1;

    if (Cell_ParamClass(a) != Cell_ParamClass(b))
        return Cell_ParamClass(a) > Cell_ParamClass(b) ? 1 : -1;

    return 0;
}


//
//  Startup_Typesets: C
//
// Create typeset variables that are defined above.
// For example: NUMBER is both integer and decimal.
// Add the new variables to the system context.
//
void Startup_Typesets(void)
{
    REBINT id;
    for (id = SYM_ANY_VALUE_Q; id != SYM_DATATYPES; id += 2) {
        REBINT n = (id - SYM_ANY_VALUE_Q) / 2;  // means Typesets[n]

        // We want the forms like ANY-SERIES? to be typechecker functions that
        // act on Typesets[n].
        //
        if (id != SYM_ANY_VALUE_Q) {  // see any-value? for unstable rule-out
            DECLARE_STABLE (typeset_index);
            Init_Integer(typeset_index, n);
            Phase* typechecker = Make_Typechecker(typeset_index);

            Init_Action(
                Force_Lib_Var(cast(SymId, id)),
                typechecker,
                Canon_Symbol(cast(SymId, id)),  // cached symbol for function
                UNBOUND
            );
        }

        // Make e.g. ANY-VALUE! a TYPE-GROUP! with the bound question mark
        // form in it, e.g. any-value!: &(any-value?)
        //
        Array* a = Alloc_Singular(NODE_FLAG_MANAGED);
        Init_Any_Word_Bound(
            Stub_Cell(a),
            REB_WORD,
            Canon_Symbol(cast(SymId, id)),
            Lib_Context,
            INDEX_ATTACHED
        );
        Init_Array_Cell(Force_Lib_Var(cast(SymId, id + 1)), REB_TYPE_GROUP, a);
    }

    Index last = (cast(int, SYM_DATATYPES) - SYM_ANY_VALUE_Q) / 2;
    assert(Typesets[last] == 0);  // table ends in zero
    UNUSED(last);
}


//
//  Shutdown_Typesets: C
//
void Shutdown_Typesets(void)
{
}


//
//  Set_Parameter_Spec: C
//
// This copies the input spec as an array stored in the parameter, while
// setting flags appropriately and making notes for optimizations to help in
// the later typechecking.
//
// 1. As written, the function spec processing code builds the parameter
//    directly into a stack variable.  That means this code can't PUSH()
//    (or call code that does).  It's not impossible to relax this and
//    have the code build the parameter into a non-stack variable then
//    copy it...but try avoiding that.
//
// 2. TAG! parameter modifiers can't be abstracted.  So you can't say:
//
//        modifier: either condition [<end>] [<maybe>]
//        foo: func [arg [modifier integer!]] [...]
//
// 3. Everything non-TAG! can be abstracted via WORD!.  This can lead to some
//    strange mixtures:
//
//        func compose/deep [x [word! (integer!)]] [ ... ]
//
//    (But then the help will show the types as [word! &integer].  Is it
//    preferable to enforce words for some things?  That's not viable for
//    type predicate actions, like SPLICE?...)
//
// 4. Ren-C disallows unbounds, and validates what the word looks up to
//    at the time of creation.  If it didn't, then optimizations could not
//    be calculated at creation-time.
//
//    (R3-Alpha had a hacky fallback where unbound variables were interpreted
//    as their word.  So if you said `word!: integer!` and used WORD!, you'd
//    get the integer typecheck... but if WORD! is unbound then it would act
//    as a WORD! typecheck.)
//
void Set_Parameter_Spec(
    Cell* param,  // target is usually a stack value [1]
    const Cell* spec,
    Specifier* spec_specifier
){
    ParamClass pclass = Cell_ParamClass(param);
    assert(pclass != PARAMCLASS_0);  // must have class

    uintptr_t* flags = &PARAMETER_FLAGS(param);
    if (*flags & PARAMETER_FLAG_REFINEMENT) {
        assert(*flags & PARAMETER_FLAG_NULL_DEFINITELY_OK);
        assert(pclass != PARAMCLASS_RETURN and pclass != PARAMCLASS_OUTPUT);
    }

    const Cell* tail;
    const Cell* item = Cell_Array_At(&tail, spec);

    Length len = tail - item;

    Array* copy = Make_Array_For_Copy(
        len,
        NODE_FLAG_MANAGED,
        Cell_Array(spec)
    );
    Set_Series_Len(copy, len);
    Cell* dest = Array_Head(copy);

    Byte* optimized = copy->misc.any.at_least_4;
    Byte* optimized_tail = optimized + sizeof(uintptr_t);

    for (; item != tail; ++item, ++dest) {
        Derelativize(dest, item, spec_specifier);
        Clear_Cell_Flag(dest, NEWLINE_BEFORE);

        if (Is_Quasiform(item)) {
            if (Cell_Heart(item) == REB_VOID) {
                *flags |= PARAMETER_FLAG_TRASH_DEFINITELY_OK;
                continue;
            }
            if (Cell_Heart(item) != REB_WORD)
                fail (item);
            *flags |= PARAMETER_FLAG_INCOMPLETE_OPTIMIZATION;
            continue;
        }
        if (Is_Quoted(item)) {
            //
            // !!! Some question on if you could do a typecheck on words like
            // an enum, e.g. `foo [size ['small 'medium 'large]]`.
            //
            fail (item);
        }

        if (Cell_Heart(item) == REB_TAG) {  // literal check of tag [2]
            bool strict = false;

            if (
                0 == CT_String(item, Root_Variadic_Tag, strict)
            ){
                // !!! The actual final notation for variadics is not decided
                // on, so there is compatibility for now with the <...> form
                // from when that was a TAG! vs. a 5-element TUPLE!  While
                // core sources were changed to `<variadic>`, asking users
                // to shuffle should only be done once (when final is known).
                //
                *flags |= PARAMETER_FLAG_VARIADIC;
                Init_Quasi_Word(dest, Canon(VARIADIC_Q)); // !!!
            }
            else if (0 == CT_String(item, Root_End_Tag, strict)) {
                *flags |= PARAMETER_FLAG_ENDABLE;
                Init_Quasi_Word(dest, Canon(NULL));  // !!!
                *flags |= PARAMETER_FLAG_NULL_DEFINITELY_OK;
            }
            else if (0 == CT_String(item, Root_Maybe_Tag, strict)) {
                *flags |= PARAMETER_FLAG_NOOP_IF_VOID;
                Set_Cell_Flag(dest, PARAMSPEC_SPOKEN_FOR);
                Init_Quasi_Word(dest, Canon(VOID));  // !!!
            }
            else if (0 == CT_String(item, Root_Opt_Tag, strict)) {
                Init_Quasi_Word(dest, Canon(NULL));  // !!!
                *flags |= PARAMETER_FLAG_NULL_DEFINITELY_OK;
            }
            else if (0 == CT_String(item, Root_Void_Tag, strict)) {
                Init_Any_Word_Bound(  // !!
                    dest,
                    REB_WORD,
                    Canon(VOID_Q),
                    Lib_Context,
                    INDEX_ATTACHED
                );
                *flags |= PARAMETER_FLAG_INCOMPLETE_OPTIMIZATION;
            }
            else if (0 == CT_String(item, Root_Skip_Tag, strict)) {
                if (pclass != PARAMCLASS_HARD)
                    fail ("Only hard-quoted parameters are <skip>-able");

                *flags |= PARAMETER_FLAG_SKIPPABLE;
                *flags |= PARAMETER_FLAG_ENDABLE; // skip => null
                Init_Quasi_Word(dest, Canon(NULL));  // !!!
                *flags |= PARAMETER_FLAG_NULL_DEFINITELY_OK;
            }
            else if (0 == CT_String(item, Root_Const_Tag, strict)) {
                *flags |= PARAMETER_FLAG_CONST;
                Set_Cell_Flag(dest, PARAMSPEC_SPOKEN_FOR);
                Init_Quasi_Word(dest, Canon(CONST));
            }
            else if (0 == CT_String(item, Root_Unrun_Tag, strict)) {
                // !!! Currently just commentary, degrading happens due
                // to type checking.  Review this.
                //
                Init_Quasi_Word(dest, Canon(UNRUN));
            }
            else {
                fail (item);
            }
            continue;
        }

        const Cell* lookup;
        if (Cell_Heart(item) == REB_WORD) {  // allow abstraction [3]
            lookup = try_unwrap(Lookup_Word(item, spec_specifier));
            if (not lookup)  // not even bound to anything
                fail (item);
            if (Is_Trash(lookup)) {  // bound but not set
                //
                // !!! This happens on things like LOGIC?, because they are
                // assigned in usermode code.  That misses an optimization
                // opportunity...suggesting strongly those be done sooner.
                //
                *flags |= PARAMETER_FLAG_INCOMPLETE_OPTIMIZATION;
                continue;
            }
            if (Is_Antiform(lookup) and Cell_Heart(lookup) != REB_FRAME)
                fail (item);
            if (Is_Quoted(lookup))
                fail (item);
        }
        else
            lookup = item;

        enum Reb_Kind heart = Cell_Heart(lookup);

        if (heart == REB_TYPE_WORD) {
            if (optimized == optimized_tail and item != tail) {
                *flags |= PARAMETER_FLAG_INCOMPLETE_OPTIMIZATION;
                continue;
            }
            Option(SymId) id = Cell_Word_Id(lookup);
            if (not IS_KIND_SYM(id))
                fail (item);
            *optimized = KIND_FROM_SYM(unwrap(id));
            ++optimized;
            Set_Cell_Flag(dest, PARAMSPEC_SPOKEN_FOR);
        }
        else if (
            heart == REB_TYPE_GROUP or heart == REB_TYPE_BLOCK
            or heart == REB_TYPE_PATH or heart == REB_TYPE_TUPLE
        ){
            *flags |= PARAMETER_FLAG_INCOMPLETE_OPTIMIZATION;
        }
        else if (heart == REB_FRAME and QUOTE_BYTE(lookup) == ANTIFORM_0) {
            Phase* phase = ACT_IDENTITY(VAL_ACTION(lookup));
            if (ACT_DISPATCHER(phase) == &Intrinsic_Dispatcher) {
                Intrinsic* intrinsic = Extract_Intrinsic(phase);
                if (intrinsic == &N_any_value_q)
                    *flags |= PARAMETER_FLAG_ANY_VALUE_OK;
                else if (intrinsic == &N_any_atom_q)
                    *flags |= PARAMETER_FLAG_ANY_ATOM_OK;
                else if (intrinsic == &N_nihil_q)
                    *flags |= PARAMETER_FLAG_NIHIL_DEFINITELY_OK;
                else
                    *flags |= PARAMETER_FLAG_INCOMPLETE_OPTIMIZATION;
            }
            else
                *flags |= PARAMETER_FLAG_INCOMPLETE_OPTIMIZATION;
        }
        else {
            // By pre-checking we can avoid needing to double check in the
            // actual type-checking phase.

            fail (item);
        }
    }

    if (optimized != optimized_tail)
        *optimized = 0;  // signal termination (else tail is termination)

    Freeze_Array_Shallow(copy);  // !!! copy and freeze should likely be deep
    INIT_CELL_PARAMETER_SPEC(param, copy);

    assert(Not_Cell_Flag(param, VAR_MARKED_HIDDEN));
}


//
//  unspecialized?: native/intrinsic [
//
//  "Tells you if argument is parameter antiform, used for unspecialized args"
//
//      return: [logic?]
//      value
//  ]
//
DECLARE_INTRINSIC(unspecialized_q)
{
    UNUSED(phase);

    Init_Logic(out, Is_Unspecialized(arg));
}


//
//  MAKE_Parameter: C
//
Bounce MAKE_Parameter(
    Level* level_,
    enum Reb_Kind kind,
    Option(Value(const*)) parent,
    const REBVAL *arg
){
    UNUSED(kind);
    UNUSED(parent);
    return RAISE(Error_Bad_Make(REB_PARAMETER, arg));
}


//
//  TO_Parameter: C
//
Bounce TO_Parameter(Level* level_, enum Reb_Kind kind, const REBVAL *arg)
{
    return MAKE_Parameter(level_, kind, nullptr, arg);
}


//
//  MF_Parameter: C
//
void MF_Parameter(REB_MOLD *mo, NoQuote(const Cell*) v, bool form)
{
    if (not form) {
        Pre_Mold(mo, v);  // #[parameter! or make parameter!
    }

    DECLARE_LOCAL(temp);
    Option(const Array*) param_array = Cell_Parameter_Spec(v);
    if (param_array)
        Init_Block(temp, unwrap(param_array));
    else
        Init_Block(temp, EMPTY_ARRAY);

    Push_GC_Guard(temp);
    Mold_Or_Form_Value(mo, temp, form);
    Drop_GC_Guard(temp);

    if (not form) {
        End_Mold(mo);
    }
}


//
//  REBTYPE: C
//
REBTYPE(Parameter)
{
    Value(*) param = D_ARG(1);
    Option(SymId) symid = Symbol_Id(verb);

    switch (symid) {

    //=//// PICK* (see %sys-pick.h for explanation) ////////////////////////=//

      case SYM_PICK_P: {
        INCLUDE_PARAMS_OF_PICK_P;
        UNUSED(ARG(location));

        const Cell* picker = ARG(picker);
        if (not Is_Word(picker))
            fail (picker);

        switch (Cell_Word_Id(picker)) {
          case SYM_TEXT: {
            Option(const String*) string = Cell_Parameter_String(param);
            if (not string)
                return nullptr;
            return Init_Text(OUT, unwrap(string)); }

          case SYM_SPEC: {
            Option(const Array*) spec = Cell_Parameter_Spec(param);
            if (not spec)
                return nullptr;
            return Init_Block(OUT, unwrap(spec)); }

          case SYM_TYPE:
            return nullptr;  // TBD

          default:
            break;
        }

        fail (Error_Bad_Pick_Raw(picker)); }


    //=//// POKE* (see %sys-pick.h for explanation) ////////////////////////=//

      case SYM_POKE_P: {
        INCLUDE_PARAMS_OF_POKE_P;
        UNUSED(ARG(location));

        const Cell* picker = ARG(picker);
        if (not Is_Word(picker))
            fail (picker);

        REBVAL *setval = ARG(value);

        switch (Cell_Word_Id(picker)) {
          case SYM_TEXT: {
            if (not Is_Text(setval))
                fail (setval);
            String* string = Copy_String_At(setval);
            Manage_Series(string);
            Freeze_Series(string);
            Set_Parameter_String(param, string);
            return COPY(param); }  // update to container (e.g. varlist) needed

          default:
            break;
        }

        fail (Error_Bad_Pick_Raw(picker)); }

      default:
        break;
    }

    fail (UNHANDLED);
}
