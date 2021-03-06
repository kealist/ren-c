//
//  File: %n-data.c
//  Summary: "native functions for data and context"
//  Section: natives
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

#include "sys-core.h"


static REBOOL Check_Char_Range(const REBVAL *val, REBINT limit)
{
    if (IS_CHAR(val))
        return not (VAL_CHAR(val) > limit);

    if (IS_INTEGER(val))
        return not (VAL_INT64(val) > limit);

    assert(ANY_STRING(val));

    REBCNT len = VAL_LEN_AT(val);
    REBCHR(const *) up = VAL_UNI_AT(val);

    for (; len > 0; len--) {
        REBUNI c;
        up = NEXT_CHR(&c, up);

        if (c > limit)
            return FALSE;
    }

    return TRUE;
}


//
//  ascii?: native [
//
//  {Returns TRUE if value or string is in ASCII character range (below 128).}
//
//      value [any-string! char! integer!]
//  ]
//
REBNATIVE(ascii_q)
{
    INCLUDE_PARAMS_OF_ASCII_Q;

    return R_FROM_BOOL(Check_Char_Range(ARG(value), 0x7f));
}


//
//  latin1?: native [
//
//  {Returns TRUE if value or string is in Latin-1 character range (below 256).}
//
//      value [any-string! char! integer!]
//  ]
//
REBNATIVE(latin1_q)
{
    INCLUDE_PARAMS_OF_LATIN1_Q;

    return R_FROM_BOOL(Check_Char_Range(ARG(value), 0xff));
}


//
//  as-pair: native [
//
//  "Combine X and Y values into a pair."
//
//      x [any-number!]
//      y [any-number!]
//  ]
//
REBNATIVE(as_pair)
{
    INCLUDE_PARAMS_OF_AS_PAIR;

    REBVAL *x = ARG(x);
    REBVAL *y = ARG(y);

    SET_PAIR(
        D_OUT,
        IS_INTEGER(x) ? VAL_INT64(x) : VAL_DECIMAL(x),
        IS_INTEGER(y) ? VAL_INT64(y) : VAL_DECIMAL(y)
    );

    return R_OUT;
}


//
//  bind: native [
//
//  "Binds words or words in arrays to the specified context."
//
//      value [any-array! any-word!]
//          "A word or array (modified) (returned)"
//      target [any-word! any-context!]
//          "The target context or a word whose binding should be the target"
//      /copy
//          "Bind and return a deep copy of a block, don't modify original"
//      /only
//          "Bind only first block (not deep)"
//      /new
//          "Add to context any new words found"
//      /set
//          "Add to context any new set-words found"
//  ]
//
REBNATIVE(bind)
{
    INCLUDE_PARAMS_OF_BIND;

    REBVAL *v = ARG(value);
    REBVAL *target = ARG(target);

    REBCNT flags = REF(only) ? BIND_0 : BIND_DEEP;

    REBU64 bind_types = TS_ANY_WORD;

    REBU64 add_midstream_types;
    if (REF(new)) {
        add_midstream_types = TS_ANY_WORD;
    }
    else if (REF(set)) {
        add_midstream_types = FLAGIT_KIND(REB_SET_WORD);
    }
    else
        add_midstream_types = 0;

    REBCTX *context;

    // !!! For now, force reification before doing any binding.

    if (ANY_CONTEXT(target)) {
        //
        // Get target from an OBJECT!, ERROR!, PORT!, MODULE!, FRAME!
        //
        context = VAL_CONTEXT(target);
    }
    else {
        assert(ANY_WORD(target));
        if (IS_WORD_UNBOUND(target))
            fail (Error_Not_Bound_Raw(target));

        context = VAL_WORD_CONTEXT(target);
    }

    if (ANY_WORD(v)) {
        //
        // Bind a single word

        if (Try_Bind_Word(context, v)) {
            Move_Value(D_OUT, v);
            return R_OUT;
        }

        // not in context, bind/new means add it if it's not.
        //
        if (REF(new) or (IS_SET_WORD(v) and REF(set))) {
            Append_Context(context, v, NULL);
            Move_Value(D_OUT, v);
            return R_OUT;
        }

        fail (Error_Not_In_Context_Raw(v));
    }

    assert(ANY_ARRAY(v));

    RELVAL *at;
    if (REF(copy)) {
        REBARR *copy = Copy_Array_Core_Managed(
            VAL_ARRAY(v),
            VAL_INDEX(v), // at
            VAL_SPECIFIER(v),
            ARR_LEN(VAL_ARRAY(v)), // tail
            0, // extra
            ARRAY_FLAG_FILE_LINE, // flags
            TS_ARRAY // types to copy deeply
        );
        at = ARR_HEAD(copy);
        Init_Any_Array(D_OUT, VAL_TYPE(v), copy);
    }
    else {
        at = VAL_ARRAY_AT(v); // only affects binding from current index
        Move_Value(D_OUT, v);
    }

    Bind_Values_Core(
        at,
        context,
        bind_types,
        add_midstream_types,
        flags
    );

    return R_OUT;
}


//
//  use: native [
//
//  {Defines words local to a block.}
//
//      return: [<opt> any-value!]
//      vars [block! word!]
//          {Local word(s) to the block}
//      body [block!]
//          {Block to evaluate}
//  ]
//
REBNATIVE(use)
//
// !!! R3-Alpha's USE was written in userspace and was based on building a
// CLOSURE! that it would DO.  Hence it took advantage of the existing code
// for tying function locals to a block, and could be relatively short.  This
// was wasteful in terms of creating an unnecessary function that would only
// be called once.  The fate of CLOSURE-like semantics is in flux in Ren-C
// (how much automatic-gathering and indefinite-lifetime will be built-in),
// yet it's also more efficient to just make a native.
//
// As it stands, the code already existed for loop bodies to do this more
// efficiently.  The hope is that with virtual binding, such constructs will
// become even more efficient--for loops, BIND, and USE.
//
// !!! Should USE allow LIT-WORD!s to mean basically a no-op, just for common
// interface with the loops?
{
    INCLUDE_PARAMS_OF_USE;

    REBCTX *context;
    Virtual_Bind_Deep_To_New_Context(
        ARG(body), // may be replaced with rebound copy, or left the same
        &context, // winds up managed; if no references exist, GC is ok
        ARG(vars) // similar to the "spec" of a loop: WORD!/LIT-WORD!/BLOCK!
    );

    if (Do_Any_Array_At_Throws(D_OUT, ARG(body)))
        return R_OUT_IS_THROWN;

    return R_OUT;
}


//
//  Get_Context_Of: C
//
REBOOL Get_Context_Of(REBVAL *out, const REBVAL *v)
{
    switch (VAL_TYPE(v)) {
    case REB_ACTION: {
        //
        // The only examples of functions bound to contexts that exist at the
        // moment are RETURN and LEAVE.  While there are archetypal natives
        // for these functions, the REBVAL instances can contain a binding
        // to the specific frame they exit.  Assume what is desired is that
        // frame...
        //
        REBNOD *n = VAL_BINDING(v);
        if (n == UNBOUND)
            return FALSE;

        REBCTX *c;
        if (IS_CELL(n)) {
            REBFRM *f = cast(REBFRM*, n);
            c = Context_For_Frame_May_Reify_Managed(f);
        }
        else {
            assert(n->header.bits & (SERIES_FLAG_ARRAY | ARRAY_FLAG_VARLIST));
            c = cast(REBCTX*, n);
        }
        Move_Value(out, CTX_ARCHETYPE(c));
        assert(IS_FRAME(out));
        break; }

    case REB_WORD:
    case REB_SET_WORD:
    case REB_GET_WORD:
    case REB_LIT_WORD:
    case REB_REFINEMENT:
    case REB_ISSUE: {
        if (IS_WORD_UNBOUND(v))
            return FALSE;

        // Requesting the context of a word that is relatively bound may
        // result in that word having a FRAME! incarnated as a REBSER node (if
        // it was not already reified.)
        //
        // !!! In the future Reb_Context will refer to a REBNOD*, and only
        // be reified based on the properties of the cell into which it is
        // moved (e.g. OUT would be examined here to determine if it would
        // have a longer lifetime than the REBFRM* or other node)
        //
        REBCTX *c = VAL_WORD_CONTEXT(v);
        Move_Value(out, CTX_ARCHETYPE(c));
        break; }

    default:
        //
        // Will OBJECT!s or FRAME!s have "contexts"?  Or if they are passed
        // in should they be passed trough as "the context"?  For now, keep
        // things clear?
        //
        assert(FALSE);
    }

    // A FRAME! has special properties of ->phase and ->binding which
    // affect the interpretation of which layer of a function composition
    // they correspond to.  If you REDO a FRAME! value it will restart at
    // different points based on these properties.  Assume the time of
    // asking is the layer in the composition the user is interested in.
    //
    // !!! This may not be the correct answer, but it seems to work in
    // practice...keep an eye out for counterexamples.
    //
    if (IS_FRAME(out)) {
        REBCTX *c = VAL_CONTEXT(out);
        REBFRM *f = CTX_FRAME_IF_ON_STACK(c);
        if (f != NULL) {
            out->payload.any_context.phase = f->phase;
            INIT_BINDING(out, f->binding);
        }
        else {
            // !!! Assume the canon FRAME! value in varlist[0] is useful?
            //
            assert(VAL_BINDING(out) == UNBOUND); // canons have no binding
        }

        assert(
            out->payload.any_context.phase == NULL
            or GET_SER_FLAG(
                ACT_PARAMLIST(out->payload.any_context.phase),
                ARRAY_FLAG_PARAMLIST
            )
        );
    }

    return TRUE;
}


//
//  value?: native [
//
//  "Test if an optional cell contains a value (e.g. `value? ()` is FALSE)"
//
//      optional [<opt> any-value!]
//  ]
//
REBNATIVE(value_q)
{
    INCLUDE_PARAMS_OF_VALUE_Q;

    return R_FROM_BOOL(ANY_VALUE(ARG(optional)));
}


//
//  unbind: native [
//
//  "Unbinds words from context."
//
//      word [block! any-word!]
//          "A word or block (modified) (returned)"
//      /deep
//          "Process nested blocks"
//  ]
//
REBNATIVE(unbind)
{
    INCLUDE_PARAMS_OF_UNBIND;

    REBVAL *word = ARG(word);

    if (ANY_WORD(word))
        Unbind_Any_Word(word);
    else
        Unbind_Values_Core(VAL_ARRAY_AT(word), NULL, REF(deep));

    Move_Value(D_OUT, word);
    return R_OUT;
}


//
//  collect-words: native [
//
//  {Collect unique words used in a block (used for context construction).}
//
//      block [block!]
//      /deep
//          "Include nested blocks"
//      /set
//          "Only include set-words"
//      /ignore
//          "Ignore prior words"
//      hidden [any-context! block!]
//          "Words to ignore"
//  ]
//
REBNATIVE(collect_words)
{
    INCLUDE_PARAMS_OF_COLLECT_WORDS;

    REBFLGS flags;
    if (REF(set))
        flags = COLLECT_ONLY_SET_WORDS;
    else
        flags = COLLECT_ANY_WORD;

    if (REF(deep))
        flags |= COLLECT_DEEP;

    UNUSED(REF(ignore)); // implied used or unused by ARG(hidden)'s voidness

    RELVAL *head = VAL_ARRAY_AT(ARG(block));

    Init_Block(D_OUT, Collect_Unique_Words_Managed(head, flags, ARG(hidden)));
    return R_OUT;
}


//
//  get: native [
//
//  {Gets the value of a word or path, or block of words/paths.}
//
//      return: [<opt> any-value!]
//      source [blank! any-word! any-path! block!]
//          {Word or path to get, or block of words or paths (blank is no-op)}
//      /try
//          {Return blank for variables that are unset}
//  ]
//
REBNATIVE(get)
//
// Note: `get [x y] [some-var :some-unset-var]` would fail without /TRY
{
    INCLUDE_PARAMS_OF_GET;

    RELVAL *source;
    REBVAL *dest;
    REBSPC *specifier;

    REBARR *results;

    if (IS_BLOCK(ARG(source))) {
        source = VAL_ARRAY_AT(ARG(source));
        specifier = VAL_SPECIFIER(ARG(source));

        results = Make_Array(VAL_LEN_AT(ARG(source)));
        TERM_ARRAY_LEN(results, VAL_LEN_AT(ARG(source)));
        dest = SINK(ARR_HEAD(results));
    }
    else {
        // Move the argument into the single cell in the frame if it's not a
        // block, so the same enumeration-up-to-an-END marker can work on it
        // as for handling a block of items.
        //
        Move_Value(D_CELL, ARG(source));
        source = D_CELL;
        specifier = SPECIFIED;
        dest = D_OUT;
        results = NULL; // wasteful but avoids maybe-used-uninitalized warning
    }

    DECLARE_LOCAL (get_path_hack); // runs prep code, don't put inside loop

    for (; NOT_END(source); ++source, ++dest) {
        if (IS_BAR(source)) {
            //
            // `a: 10 | b: 20 | get [a | b]` will give back `[10 | 20]`.
            // While seemingly not a very useful feature standalone, this
            // compatibility with SET could come in useful so that blocks
            // don't have to be rearranged to filter out BAR!s.
            //
            Init_Bar(dest);
        }
        else if (IS_BLANK(source)) {
            Init_Void(dest); // may be turned to blank after loop, or error
        }
        else if (ANY_WORD(source)) {
            Move_Opt_Var_May_Fail(dest, source, specifier);
        }
        else if (ANY_PATH(source)) {
            //
            // `get 'foo/bar` acts like `:foo/bar`
            // Get_Path_Core() will raise an error if there are any GROUP!s.
            //
            Get_Path_Core(dest, source, specifier);
        }

        if (IS_VOID(dest)) {
            if (REF(try))
                Init_Blank(dest);
            else {
                if (IS_BLOCK(ARG(source))) // can't put voids in blocks
                    fail (Error_No_Value_Core(source, specifier));
            }
        }
    }

    if (IS_BLOCK(ARG(source)))
        Init_Block(D_OUT, results);

    return R_OUT;
}


//
//  try: native [
//
//  {Turns nulls into blanks, ANY-VALUE! passes through. (See Also: OPT)}
//
//      return: [any-value!]
//          {blank if input was void, or original value otherwise}
//      optional [<opt> any-value!]
//  ]
//
REBNATIVE(try)
{
    INCLUDE_PARAMS_OF_TRY;

    if (IS_VOID(ARG(optional)))
        return R_BLANK;

    Move_Value(D_OUT, ARG(optional));
    return R_OUT;
}


//
//  opt: native [
//
//  {Convert blanks to optionals, other values pass through (See Also: TRY)}
//
//      return: [<opt> any-value!]
//          {void if input was a BLANK!, or original value otherwise}
//      value [<opt> any-value!]
//  ]
//
REBNATIVE(opt)
{
    INCLUDE_PARAMS_OF_OPT;

    if (IS_BLANK(ARG(value)))
        return R_VOID;

    Move_Value(D_OUT, ARG(value));
    return R_OUT;
}


//
//  in: native [
//
//  "Returns the word or block bound into the given context."
//
//      context [any-context! block!]
//      word [any-word! block! group!] "(modified if series)"
//  ]
//
REBNATIVE(in)
//
// !!! The argument names here are bad... not necessarily a context and not
// necessarily a word.  `code` or `source` to be bound in a `target`, perhaps?
{
    INCLUDE_PARAMS_OF_IN;

    REBVAL *val = ARG(context); // object, error, port, block
    REBVAL *word = ARG(word);

    DECLARE_LOCAL (safe);

    if (IS_BLOCK(val) || IS_GROUP(val)) {
        if (IS_WORD(word)) {
            const REBVAL *v;
            REBCNT i;
            for (i = VAL_INDEX(val); i < VAL_LEN_HEAD(val); i++) {
                Get_Simple_Value_Into(
                    safe,
                    VAL_ARRAY_AT_HEAD(val, i),
                    VAL_SPECIFIER(val)
                );

                v = safe;
                if (IS_OBJECT(v)) {
                    REBCTX *context = VAL_CONTEXT(v);
                    REBCNT index = Find_Canon_In_Context(
                        context, VAL_WORD_CANON(word), FALSE
                    );
                    if (index != 0) {
                        INIT_WORD_CONTEXT(word, context);
                        INIT_WORD_INDEX(word, index);
                        Move_Value(D_OUT, word);
                        return R_OUT;
                    }
                }
            }
            return R_BLANK;
        }

        fail (Error_Invalid(word));
    }

    REBCTX *context = VAL_CONTEXT(val);

    // Special form: IN object block
    if (IS_BLOCK(word) || IS_GROUP(word)) {
        Bind_Values_Deep(VAL_ARRAY_HEAD(word), context);
        Move_Value(D_OUT, word);
        return R_OUT;
    }

    REBCNT index = Find_Canon_In_Context(context, VAL_WORD_CANON(word), FALSE);
    if (index == 0)
        return R_BLANK;

    Init_Any_Word_Bound(
        D_OUT,
        VAL_TYPE(word),
        VAL_WORD_SPELLING(word),
        context,
        index
    );
    return R_OUT;
}


//
//  resolve: native [
//
//  {Copy context by setting values in the target from those in the source.}
//
//      target [any-context!] "(modified)"
//      source [any-context!]
//      /only
//          "Only specific words (exports) or new words in target"
//      from [block! integer!]
//          "(index to tail)"
//      /all
//          "Set all words, even those in the target that already have a value"
//      /extend
//          "Add source words to the target if necessary"
//  ]
//
REBNATIVE(resolve)
{
    INCLUDE_PARAMS_OF_RESOLVE;

    if (IS_INTEGER(ARG(from))) {
        // check range and sign
        Int32s(ARG(from), 1);
    }

    UNUSED(REF(only)); // handled by noticing if ARG(from) is void
    Resolve_Context(
        VAL_CONTEXT(ARG(target)),
        VAL_CONTEXT(ARG(source)),
        ARG(from),
        REF(all),
        REF(extend)
    );

    Move_Value(D_OUT, ARG(target));
    return R_OUT;
}


//
//  set: native [
//
//  {Sets a word, path, or block of words and paths to specified value(s).}
//
//      return: [<opt> any-value!]
//          {Will be the values set to, or void if any set values are void}
//      target [any-word! any-path! block!]
//          {Word or path, or block of words and paths}
//      value [<opt> any-value!]
//          "Value or block of values"
//      /single
//          {If target and value are blocks, set each item to the same value}
//      /some
//          {Blank values (or values past end of block) are not set.}
//      /enfix
//          {Function calls through this word should get first arg from left}
//  ]
//
REBNATIVE(set)
//
// Blocks are supported as:
//
//     >> set [a b] [1 2]
//     >> print a
//     1
//     >> print b
//     2
{
    INCLUDE_PARAMS_OF_SET;

    const RELVAL *value;
    REBSPC *value_specifier;

    const RELVAL *target;
    REBSPC *target_specifier;

    REBOOL single;
    if (IS_BLOCK(ARG(target))) {
        //
        // R3-Alpha and Red let you write `set [a b] 10`, since the thing
        // you were setting to was not a block, would assume you meant to set
        // all the values to that.  BUT since you can set things to blocks,
        // this has a bad characteristic of `set [a b] [10]` being treated
        // differently, which can bite you if you `set [a b] value` for some
        // generic value.
        //
        if (IS_BLOCK(ARG(value)) and not REF(single)) {
            //
            // There is no need to check values for voidness in this case,
            // since arrays cannot contain voids.
            //
            value = VAL_ARRAY_AT(ARG(value));
            value_specifier = VAL_SPECIFIER(ARG(value));
            single = FALSE;
        }
        else {
            value = ARG(value);
            value_specifier = SPECIFIED;
            single = TRUE;
        }

        target = VAL_ARRAY_AT(ARG(target));
        target_specifier = VAL_SPECIFIER(ARG(target));
    }
    else {
        Move_Value(D_CELL, ARG(target));
        target = D_CELL;
        target_specifier = SPECIFIED;

        // Use the fact that D_CELL is implicitly terminated so that the
        // loop below can share code between `set [a b] x` and `set a x`, by
        // incrementing the target pointer and hitting an END marker
        //
        assert(ANY_WORD(target) or ANY_PATH(target) or IS_BAR(target));

        value = ARG(value);
        value_specifier = SPECIFIED;
        single = TRUE;
    }

    DECLARE_LOCAL (set_path_hack); // runs prep code, don't put inside loop

    for (
        ;
        NOT_END(target);
        ++target, (single or IS_END(value)) ? NOOP : (++value, NOOP)
     ){
        if (REF(some)) {
            if (IS_END(value))
                break; // won't be setting any further values
            if (IS_BLANK(value))
                continue;
        }

        if (REF(enfix) and not IS_ACTION(ARG(value)))
            fail ("Attempt to SET/ENFIX on a non-function");

        if (IS_BAR(target)) {
            //
            // Just skip it, e.g. `set [a | b] [1 2 3]` sets a to 1, and b
            // to 3, but drops the 2.  This functionality was achieved
            // initially with blanks, but with setting in particular there
            // are cases of `in obj 'word` which give back blank if the word
            // is not there, so it leads to too many silent errors.
        }
        else if (ANY_WORD(target)) {
            REBVAL *var = Sink_Var_May_Fail(target, target_specifier);
            Derelativize(
                var,
                IS_END(value) ? BLANK_VALUE : value,
                value_specifier
            );
            if (REF(enfix))
                SET_VAL_FLAG(var, VALUE_FLAG_ENFIXED);
        }
        else if (ANY_PATH(target)) {
            DECLARE_LOCAL (specific);
            if (IS_END(value))
                Init_Blank(specific);
            else
                Derelativize(specific, value, value_specifier);

            // `set 'foo/bar 1` acts as `foo/bar: 1`
            // Set_Path_Core() will raise an error if there are any GROUP!s
            //
            // Though you can't dispatch enfix from a path (at least not at
            // present), the flag tells it to enfix a word in a context, or
            // it will error if that's not what it looks up to.
            //
            Set_Path_Core(target, target_specifier, specific, REF(enfix));
        }
        else
            fail (Error_Invalid_Core(target, target_specifier));
    }

    Move_Value(D_OUT, ARG(value));
    return R_OUT;
}


//
//  unset: native [
//
//  {Unsets the value of a word (in its current context.)}
//
//      return: [<opt>]
//      target [any-word! block!]
//          "Word or block of words"
//  ]
//
REBNATIVE(unset)
{
    INCLUDE_PARAMS_OF_UNSET;

    REBVAL *target = ARG(target);

    if (ANY_WORD(target)) {
        REBVAL *var = Sink_Var_May_Fail(target, SPECIFIED);
        Init_Void(var);
        return R_VOID;
    }

    assert(IS_BLOCK(target));

    RELVAL *word;
    for (word = VAL_ARRAY_AT(target); NOT_END(word); ++word) {
        if (!ANY_WORD(word))
            fail (Error_Invalid_Core(word, VAL_SPECIFIER(target)));

        REBVAL *var = Sink_Var_May_Fail(word, VAL_SPECIFIER(target));
        Init_Void(var);
    }

    return R_VOID;
}


//
//  enfixed?: native [
//
//  {TRUE if looks up to a function and gets first argument before the call}
//
//      source [any-word! any-path!]
//  ]
//
REBNATIVE(enfixed_q)
{
    INCLUDE_PARAMS_OF_ENFIXED_Q;

    REBVAL *source = ARG(source);

    if (ANY_WORD(source)) {
        const REBVAL *var = Get_Var_Core(
            source, SPECIFIED, GETVAR_READ_ONLY // may fail()
        );

        assert(NOT_VAL_FLAG(var, VALUE_FLAG_ENFIXED) or IS_ACTION(var));
        return R_FROM_BOOL(GET_VAL_FLAG(var, VALUE_FLAG_ENFIXED));
    }
    else {
        assert(ANY_PATH(source));

        Get_Path_Core(D_CELL, source, SPECIFIED);
        assert(NOT_VAL_FLAG(D_CELL, VALUE_FLAG_ENFIXED) or IS_ACTION(D_CELL));
        return R_FROM_BOOL(GET_VAL_FLAG(D_CELL, VALUE_FLAG_ENFIXED));
    }
}


//
//  semiquoted?: native [
//
//  {Discern if a function parameter came from an "active" evaluation.}
//
//      parameter [word!]
//  ]
//
REBNATIVE(semiquoted_q)
//
// This operation is somewhat dodgy.  So even though the flag is carried by
// all values, and could be generalized in the system somehow to query on
// anything--we don't.  It's strictly for function parameters, and
// even then it should be restricted to functions that have labeled
// themselves as absolutely needing to do this for ergonomic reasons.
{
    INCLUDE_PARAMS_OF_SEMIQUOTED_Q;

    // !!! TBD: Enforce this is a function parameter (specific binding branch
    // makes the test different, and easier)

    const REBVAL *var = Get_Var_Core( // may fail
        ARG(parameter), SPECIFIED, GETVAR_READ_ONLY
    );
    return R_FROM_BOOL(GET_VAL_FLAG(var, VALUE_FLAG_UNEVALUATED));
}


//
//  identity: native [
//
//  {Function for returning the same value that it got in (identity function)}
//
//      return: [any-value!]
//      value [<end> any-value!]
//          {!!! <end> flag is hack to limit enfix reach to the left}
//      /quote
//          {Make it seem that the return result was quoted}
//  ]
//
REBNATIVE(identity)
//
// https://en.wikipedia.org/wiki/Identity_function
// https://stackoverflow.com/q/3136338
//
// !!! Quoting version is currently specialized as SEMIQUOTE, for convenience.
//
// This is assigned to <- for convenience, but cannot be used under that name
// in bootstrap with R3-Alpha.  It uses the <end>-ability to stop left reach,
// since there is no specific flag for that...but in actuality, it is designed
// to not accept nulls.
{
    INCLUDE_PARAMS_OF_IDENTITY;

    if (IS_VOID(ARG(value))) { // <end>
        DECLARE_LOCAL (word);
        Init_Word(word, VAL_PARAM_SPELLING(PAR(value)));
        fail (Error_No_Value(word));
    }

    Move_Value(D_OUT, ARG(value));

    if (REF(quote))
        SET_VAL_FLAG(D_OUT, VALUE_FLAG_UNEVALUATED);

    return R_OUT;
}


//
//  free: native [
//
//  {Releases the underlying data of a value so it can no longer be accessed}
//
//      return: [<opt>]
//      memory [any-series! any-context! handle!]
//  ]
//
REBNATIVE(free)
{
    INCLUDE_PARAMS_OF_FREE;

    REBVAL *v = ARG(memory);

    if (ANY_CONTEXT(v) or IS_HANDLE(v))
        fail ("FREE only implemented for ANY-SERIES! at the moment");

    REBSER *s = VAL_SERIES(v);
    if (GET_SER_INFO(s, SERIES_INFO_INACCESSIBLE))
        fail ("Cannot FREE already freed series");
    FAIL_IF_READ_ONLY_SERIES(s);

    Decay_Series(s);
    return R_VOID;
}


//
//  free?: native [
//
//  {Tells if data has been released with FREE}
//
//      return: "Returns false if value wouldn't be FREEable (e.g. LOGIC!)"
//          [logic!]
//      value [any-value!]
//  ]
//
REBNATIVE(free_q)
{
    INCLUDE_PARAMS_OF_FREE_Q;

    REBVAL *v = ARG(value);

    REBSER *s;
    if (ANY_CONTEXT(v))
        s = SER(CTX_VARLIST(VAL_CONTEXT(v)));
    else if (IS_HANDLE(v))
        s = SER(v->extra.singular);
    else if (ANY_SERIES(v))
        s = VAL_SERIES(v);
    else
        return R_FALSE;

    return R_FROM_BOOL(GET_SER_INFO(s, SERIES_INFO_INACCESSIBLE));
}


//
//  as: native [
//
//  {Aliases the underlying data of one series to act as another of same class}
//
//      return: [<opt> any-series! any-word!]
//      type [datatype!]
//      value [blank! any-series! any-word!]
//  ]
//
REBNATIVE(as)
{
    INCLUDE_PARAMS_OF_AS;

    REBVAL *v = ARG(value);
    if (IS_BLANK(v))
        return R_VOID; // "blank in, null out" convention for non-modifiers

    enum Reb_Kind new_kind = VAL_TYPE_KIND(ARG(type));

    switch (new_kind) {
    case REB_BLOCK:
    case REB_GROUP:
    case REB_PATH:
    case REB_LIT_PATH:
    case REB_GET_PATH:
        if (not ANY_ARRAY(v))
            goto bad_cast;
        break;

    case REB_TEXT:
    case REB_TAG:
    case REB_FILE:
    case REB_URL:
    case REB_EMAIL: {
        //
        // !!! Until UTF-8 Everywhere, turning ANY-WORD! into an ANY-STRING!
        // means it has to be UTF-8 decoded into REBUNI (UCS-2).  We do that
        // but make sure it is locked, so that when it does give access to
        // WORD! you won't think you can mutate the data.  (Though mutable
        // WORD! should become a thing, if they're not bound or locked.)
        //
        if (ANY_WORD(v)) {
            REBSTR *spelling = VAL_WORD_SPELLING(v);
            REBSER *string = Make_Sized_String_UTF8(
                STR_HEAD(spelling),
                STR_SIZE(spelling)
            );
            SET_SER_INFO(string, SERIES_INFO_FROZEN);
            Init_Any_Series(D_OUT, new_kind, string);
            return R_OUT;
        }

        // !!! Similarly, until UTF-8 Everywhere, we can't actually alias
        // the UTF-8 bytes in a binary as a WCHAR string.
        //
        if (IS_BINARY(v)) {
            REBSER *string = Make_Sized_String_UTF8(
                cs_cast(VAL_BIN_AT(v)),
                VAL_LEN_AT(v)
            );
            if (Is_Value_Immutable(v))
                SET_SER_INFO(string, SERIES_INFO_FROZEN);
            else {
                // !!! Catch any cases of people who were trying to alias the
                // binary, make mutations via the string, and see those
                // changes show up in the binary.  That can't work until UTF-8
                // everywhere.  Most callsites don't need the binary after
                // conversion...if so, tthey should AS a COPY of it for now.
                //
                Decay_Series(VAL_SERIES(v));
            }
            Init_Any_Series(D_OUT, new_kind, string);
            return R_OUT;
        }

        if (not ANY_STRING(v))
            goto bad_cast;
        break; }

    case REB_WORD:
    case REB_GET_WORD:
    case REB_SET_WORD:
    case REB_LIT_WORD:
    case REB_ISSUE:
    case REB_REFINEMENT: {
        //
        // !!! Until UTF-8 Everywhere, turning ANY-STRING! into an ANY-WORD!
        // means you have to have an interning of it.
        //
        if (ANY_STRING(v)) {
            //
            // Don't give misleading impression that mutations of the input
            // string will change the output word, by freezing the input.
            // This will be relaxed when mutable words exist.
            //
            Freeze_Sequence(VAL_SERIES(v));

            REBSIZ utf8_size;
            REBSIZ offset;
            REBSER *temp = Temp_UTF8_At_Managed(
                &offset, &utf8_size, v, VAL_LEN_AT(v)
            );
            Init_Any_Word(
                D_OUT,
                new_kind,
                Intern_UTF8_Managed(BIN_AT(temp, offset), utf8_size)
            );
            return R_OUT;
        }

        // !!! Since pre-UTF8-everywhere ANY-WORD! was saved in UTF-8 it would
        // be sort of possible to alias a binary as a WORD!.  But modification
        // wouldn't be allowed (as there are no mutable words), and also the
        // interning logic would have to take ownership of the binary if it
        // was read-only.  No one is converting binaries to words yet, so
        // wait to implement the logic until the appropriate time...just lock
        // the binary for now.
        //
        if (IS_BINARY(v)) {
            Freeze_Sequence(VAL_SERIES(v));
            Init_Any_Word(
                D_OUT,
                new_kind,
                Intern_UTF8_Managed(VAL_BIN_AT(v), VAL_LEN_AT(v))
            );
            return R_OUT;
        }

        if (not ANY_WORD(v))
            goto bad_cast;
        break; }

    case REB_BINARY: {
        //
        // !!! A locked BINARY! shouldn't (?) complain if it exposes a
        // REBSTR holding UTF-8 data, even prior to the UTF-8 conversion.
        //
        if (ANY_WORD(v)) {
            assert(Is_Value_Immutable(v));
            Init_Binary(D_OUT, VAL_WORD_SPELLING(v));
            return R_OUT;
        }

        if (ANY_STRING(v)) {
            REBSER *bin = Make_UTF8_From_Any_String(v, VAL_LEN_AT(v));

            // !!! Making a binary out of a UCS-2 encoded string currently
            // frees the string data if it's mutable, and if that's not
            // satisfactory you can make a copy before the AS.
            //
            if (Is_Value_Immutable(v))
                Freeze_Sequence(bin);
            else
                Decay_Series(VAL_SERIES(v));

            Init_Binary(D_OUT, bin);
            return R_OUT;
        }

        fail (v); }

    bad_cast:;

    default:
        // all applicable types should be handled above
        fail (Error_Bad_Cast_Raw(v, ARG(type)));
    }

    VAL_SET_TYPE_BITS(v, new_kind);
    Move_Value(D_OUT, v);
    return R_OUT;
}


//
//  aliases?: native [
//
//  {Return whether or not the underlying data of one value aliases another}
//
//     value1 [any-series!]
//     value2 [any-series!]
//  ]
//
REBNATIVE(aliases_q)
{
    INCLUDE_PARAMS_OF_ALIASES_Q;

    return R_FROM_BOOL(VAL_SERIES(ARG(value1)) == VAL_SERIES(ARG(value2)));
}


// Common routine for both SET? and UNSET?
//
//     SET? 'UNBOUND-WORD -> will error
//     SET? 'OBJECT/NON-MEMBER -> will return false
//     SET? 'OBJECT/NON-MEMBER/XXX -> will error
//     SET? 'DATE/MONTH -> is true, even though not a variable resolution
//
inline static REBOOL Is_Set(const REBVAL *location)
{
    if (ANY_WORD(location))
        return ANY_VALUE(Get_Opt_Var_May_Fail(location, SPECIFIED));

    DECLARE_LOCAL (temp); // result may be generated
    Get_Path_Core(temp, location, SPECIFIED);
    return ANY_VALUE(temp);
}


//
//  set?: native/body [
//
//  "Whether a bound word or path is set (!!! shouldn't eval GROUP!s)"
//
//      location [any-word! any-path!]
//  ][
//      value? get location
//  ]
//
REBNATIVE(set_q)
{
    INCLUDE_PARAMS_OF_SET_Q;

    return R_FROM_BOOL(Is_Set(ARG(location)));
}


//
//  unset?: native/body [
//
//  "Whether a bound word or path is unset (!!! shouldn't eval GROUP!s)"
//
//      location [any-word! any-path!]
//  ][
//      null? get location
//  ]
//
REBNATIVE(unset_q)
{
    INCLUDE_PARAMS_OF_UNSET_Q;

    return R_FROM_BOOL(not Is_Set(ARG(location)));
}


//
//  quote: native/body [
//
//  "Returns value passed in without evaluation."
//
//      return: {The input value, verbatim--unless /SOFT and soft quoted type}
//          [<opt> any-value!]
//      :value {Value to quote, <opt> is impossible (see UNEVAL)}
//          [any-value!]
//      /soft {Evaluate if a GROUP!, GET-WORD!, or GET-PATH!}
//  ][
//      if* soft and (match [group! get-word! get-path!] :value) [
//          eval value
//      ] else [
//          :value ;-- also sets unevaluated bit, how could a user do so?
//      ]
//  ]
//
REBNATIVE(quote)
{
    INCLUDE_PARAMS_OF_QUOTE;

    REBVAL *v = ARG(value);

    if (REF(soft) and IS_QUOTABLY_SOFT(v)) {
        Move_Value(D_CELL, v);
        return R_REEVALUATE_CELL; // EVAL's mechanic lets us reuse this frame
    }

    Move_Value(D_OUT, v);
    SET_VAL_FLAG(D_OUT, VALUE_FLAG_UNEVALUATED);
    return R_OUT;
}


//
//  null?: native/body [
//
//  "Tells you if the argument is not a value (e.g. `null? do []` is TRUE)"
//
//      optional [<opt> any-value!]
//  ][
//      () = type of :optional
//  ]
//
REBNATIVE(null_q)
{
    INCLUDE_PARAMS_OF_NULL_Q;

    return R_FROM_BOOL(IS_VOID(ARG(optional)));
}


//
//  nothing?: native/body [
//
//  "Returns TRUE if argument is either a NONE! or no value is passed in"
//
//      value [<opt> any-value!]
//  ][
//      any [
//          unset? 'value
//          blank? :value
//      ]
//  ]
//
REBNATIVE(nothing_q)
{
    INCLUDE_PARAMS_OF_NOTHING_Q;

    return R_FROM_BOOL(IS_BLANK(ARG(value)) or IS_VOID(ARG(value)));
}


//
//  something?: native/body [
//
//  "Returns TRUE if a value is passed in and it isn't a NONE!"
//
//      value [<opt> any-value!]
//  ][
//      all [
//          set? 'value
//          not blank? value
//      ]
//  ]
//
REBNATIVE(something_q)
{
    INCLUDE_PARAMS_OF_SOMETHING_Q;

    return R_FROM_BOOL(not (IS_BLANK(ARG(value)) or IS_VOID(ARG(value))));
}
