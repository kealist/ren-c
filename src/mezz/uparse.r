Rebol [
    Title: {UPARSE: Usermode Implementation of PARSE in Ren-C}
    License: {LGPL 3.0}

    Type: module
    Name: Usermode-PARSE

    Exports: [
        combinator
        parse  default-combinators
        using  ; TBD: will become new meaning of USE
    ]

    Description: {
        Rebol's PARSE is a tool for performing RegEx-style tasks using an
        English-like dialect.  It permits breaking down complex expressions
        into named subrules, and has a very freeform model for escaping into
        and back out of imperative code:

        http://www.rebol.com/docs/core23/rebolcore-15.html
        http://www.codeconscious.com/rebol/parse-tutorial.html
        https://www.red-lang.org/2013/11/041-introducing-parse.html

        The implementation of PARSE engines has traditionally been as
        optimized systems-level code (e.g. C or Red/System), built into the
        interpreter.  This does not offer flexibility to make any minor or
        major changes to the dialect that a user might imagine.

        This script attempts to make PARSE more "hackable", by factoring the
        implementation out so that each keyword or data behavior is handled
        by an individual `parser` function.  The parameters to this function
        are stylized so that the parse engine can compose these smaller
        parsers together as part of larger processing operations.  This
        approach is typically called "Parser Combinators":

        https://en.wikipedia.org/wiki/Parser_combinator

        While this overlaps with typical parser combinators, Rebol's design
        affords it a unique spin.  By building the backbone of the dialect as
        a BLOCK!, there's no SEQUENCE or ALTERNATIVE combinators.  Instead,
        blocks make sequencing implicit just by steps being ordered after one
        another.  The alternates are managed by means of `|` markers, which
        are detected by the implementation of the block combinator--and `|` is
        not a combinator in its own right.  With novel operators and convenient
        ways of switching into imperative processing, it gets a unique and
        literate feel with a relatively clean appearance.

        By making the combinator list passed to UPARSE as a MAP!, is possible
        to easily create overrides or variations of the dialect.  (For
        instance, a version that is compatible with older Rebols.)  But the
        goal is to facilitate even more ambitious features.
    }

    Notes: {
        * This implementation will be *extremely* slow for the foreseeable
          future.  But since it is built on usermode facilities, any
          optimizations that are applied to it will bring systemic benefits.
          Ultimately the goal is to merge this architecture in with the
          "messier" C code...hopefully preserving enough of the hackability
          while leveraging low-level optimizations where possible.
    }
]


; All combinators receive the INPUT to be processed.  They are also given an
; object representing the STATE of the parse (currently that is just the
; FRAME! of the main UPARSE call which triggered the operation, so any of the
; locals or parameters to that can be accessed, e.g. the list of /COMBINATORS)
;
; The goal of a combinator is to decide whether to match (by returning a
; new position in the series) or fail to match (by returning a NULL)
;
; Additional parameters to a combinator are fulfilled by the parse engine by
; looking at the ensuing rules in the rule block.
;
; One of the parameter types that can be given to these functions are another
; parser, to combine them (hence "parser combinator").  So you can take a
; combinator like OPT and parameterize it with SOME which is parameterized
; with "A", to get the parser `opt some "a"`.
;
; But if a parameter to a combinator is marked as quoted, then that will take
; a value from the callsite literally.
;
; A special COMBINATOR generator is used.  This saves on repetition of
; parameters and also lets the engine get its hooks into the execution of
; parsers...for instance to diagnose the furthest point the parsing reached.

; !!! COMBINATOR is used so heavily that having it be usermode makes UPARSE
; prohibitively slow.  So it was made native.  However the usermode version is
; kept as a proof of concept that a user *could* have made such a thing.  It
; should be swapped in occasionally in the tests to overwrite the native
; version just to keep tabs on it.
;
; We would like it to be possible to write an "easy" combinator that does not
; do piping of the pending elements, but takes it for granted as coming from
; the success or failure of each parser called in order.
;
; It's hard to be able to tell which it would be, but...hm...maybe not so hard.
; If you formally declare a return value for the `pending:` then that means
; you're going to manage it yourself, otherwise it will be automagic.

combinator: func [
    {Make a stylized ACTION! that fulfills the interface of a combinator}

    return: [action!]

    spec [block!]
    body [block!]

    <static> wrapper (
        func [
            {Enclosing function for hooking all combinators}
            return: [<opt> <void> any-value!]
            f [frame!]
        ][
            ; This hook lets us run code before and after each execution of
            ; the combinator.  That offers lots of potential, but for now
            ; we just use it to notice the furthest parse point reached.
            ;
            let state: f.state

            let result': ^ eval f  ; want to detect "pure void" (/MAYBE)
            if state.verbose [
                print ["RESULT':" (mold result' else ["NULL"])]
            ]
            return unmeta (result' also [
                all [  ; if success, mark state.furthest
                    state.furthest
                    (index? f.remainder) > (index? state.furthest)
                    state.furthest: f.remainder
                ]
            ])
        ]
    )
][
    let autopipe: ~

    let action: func compose [
        ; Get the text description if given
        ((if text? spec.1 [spec.1, elide spec: my next]))

        ; Enforce a RETURN: definition.
        ;
        ((
            assert [spec.1 = 'return:]
            assert [text? spec.2]
            assert [block? spec.3]

            reduce [spec.1 spec.2 spec.3]
            elide spec: my skip 3
        ))

        remainder: [<opt> any-series!]  ; all combinators have remainder

        ((if spec.1 = 'pending: [
            assert [spec.2 = [blank! block!]]
            autopipe: false  ; they're asking to handle pending themselves
            reduce [spec.1 spec.2]
            elide spec: my skip 2
        ] else [
            autopipe: true  ; they didn't mention pending, handle automatically
            [pending: [blank! block!]]
        ]))

        state [frame!]
        input [any-series!]

        ; Whatever arguments the combinator takes, if any
        ;
        ((spec))
    ] compose [
        ;
        ; !!! If we are "autopipe" then we need to make it so the parsers that
        ; we receive in will automatically bubble up their pending contents in
        ; order of being called.

        ((if autopipe '[
            let f: binding of 'return

            pending: _
            let in-args: false
            for-each [key val] f [
                if not in-args [
                    if key = 'input [in-args: true]
                    continue
                ]
                all [
                    not unset? 'val
                    action? :val
                ] then [
                    ; All parsers passed as arguments, we want it to be
                    ; rigged so that their results append to an aggregator in
                    ; the order they are run (if they succeed).
                    ;
                    f.(key): enclose (augment :val [/modded]) func [
                        return: [<opt> <void> any-value!]
                        f2
                    ][
                        f2.pending: let subpending
                        return unmeta ((^ eval f2) also [
                            pending: glom pending subpending
                        ])
                    ]
                ]
            ]
        ]))

        return (as group! body)

        ; If the body does not return and falls through here, the function
        ; will fail as it has a RETURN: that needs to be used to type check
        ; (and must always be used so all return paths get covered if hooked)
    ]

    ; Enclosing with the wrapper permits us to inject behavior before and
    ; after each combinator is executed.
    ;
    return enclose :action :wrapper
]


; It should be possible to find out if something is a combinator in a more
; rigorous way than this.  But just check the parameters for now.
;
combinator?: func [
    {Crude test to try and determine if an ACTION! is a combinator}
    return: [logic!]
    action [action!]
    <local> keys
][
    keys: words of :action  ; test if it's a combinator
    return did all [
        find keys quote 'remainder
        find keys quote 'input
        find keys quote 'state
    ]
]


; !!! We use a MAP! here instead of an OBJECT! because if it were MAKE OBJECT!
; then the parse keywords would override the Rebol functions (so you couldn't
; use ANY inside the implementation of a combinator, because there's a
; combinator named ANY).  This is part of the general issues with binding that
; need to have answers.
;
default-combinators: make map! reduce [

    === BASIC KEYWORDS ===

    'maybe combinator [
        {If applying parser fails, succeed and return VOID; don't advance input}
        return: "PARSER's result if it succeeds w/non-NULL, otherwise VOID"
            [<opt> <void> any-value!]
        parser [action!]
        <local> result'
    ][
        ([^result' remainder]: parser input) then [
            any [
                result' = '~null~  ; `maybe if true [null]` vanishes
                result' = '~       ; `maybe if true [if false [<a>]]` vanishes
            ] then [
                return void
            ]
            return unmeta result'  ; return successful parser result
        ]
        remainder: input  ; succeed on parser failure but don't advance input
        return void  ; void result (vanishes, or leaves SET-WORD!s unchanged)
    ]

    'opt combinator [
        {If applying parser fails, succeed and return NULL; don't advance input}
        return: "PARSER's result if it succeeds, otherwise NULL"
            [<opt> any-value!]
        parser [action!]
        <local> result'
    ][
        ([^result' remainder]: parser input) then [
            return unmeta result'  ; return successful parser result
        ]
        remainder: input  ; succeed on parser failure but don't advance input
        return heavy null  ; "heavy null" result (plain NULL signals failure)
    ]

    'not combinator [
        {Fail if the parser rule given succeeds, else continue}
        return: "~not~ if the rule fails, NULL if it succeeds"
            [<opt> bad-word!]
        parser [action!]
    ][
        ([^ remainder]: parser input) then [  ; don't care about result
            return null
        ]
        remainder: input  ; parser failed, so NOT reports success
        return ~not~  ; clearer than returning NULL
    ]

    'ahead combinator [
        {Leave the parse position at the same location, but fail if no match}
        return: "parser result if success, NULL if failure"
            [<opt> any-value!]
        parser [action!]
    ][
        remainder: input
        return parser input  ; don't care about what parser's remainder is
    ]

    'further combinator [
        {Pass through the result only if the input was advanced by the rule}
        return: "parser result if it succeeded and advanced input, else NULL"
            [<opt> any-value!]
        parser [action!]
        <local> result' pos
    ][
        ([^result' pos]: parser input) else [
            return null  ; the parse rule did not match
        ]
        if (index? pos) <= (index? input) [
            return null  ; the rule matched, but did not advance the input
        ]
        remainder: pos
        return unmeta result'
    ]

    === LOOPING CONSTRUCT KEYWORDS ===

    ; UPARSE uses SOME as its looping operator, with OPT SOME or MAYBE SOME
    ; taking the place of both ANY and WHILE.  ANY is reserved for more fitting
    ; semantics for the word, and WHILE is reclaimed as an arity-2 construct.
    ; The progress requirement that previously made ANY different from WHILE is
    ; achieved with OPT SOME FURTHER:
    ;
    ; https://forum.rebol.info/t/1540/12
    ;
    ; Note that TALLY can be used as a substitute for WHILE if the rule product
    ; isn't of interest.
    ;
    ; https://forum.rebol.info/t/1581/2

    'some combinator [
        {Run the parser argument in a loop, requiring at least one match}
        return: "Result of last successful match"
            [<opt> any-value!]
        parser [action!]
        <local> last-result' result' pos
    ][
        append state.loops binding of 'return

        ([^last-result' input]: parser input) else [
            take/last state.loops
            return null
        ]
        cycle [  ; if first try succeeds, we'll succeed overall--keep looping
            ([^result' pos]: parser input) else [
                take/last state.loops
                remainder: input
                return unmeta last-result'
            ]
            last-result': result'
            input: pos
        ]
        fail ~unreachable~
    ]

    'while combinator [
        {Run the body parser in a loop, for as long as condition matches}
        return: "Result of last body parser (or none if failure)"
            [<opt> any-value!]
        condition-parser [action!]
        body-parser [action!]
        <local> result' last-result' pos
    ][
        append state.loops binding of 'return

        last-result': @void

        cycle [
            ([# pos]: condition-parser input) else [
                take/last state.loops
                remainder: input
                return unmeta last-result'
            ]

            input: pos

            ; We don't worry about the body parser's success or failure, but
            ; we do want to update the position and last result on success.
            ;
            ([^result' pos]: body-parser input) then [
                input: pos
                last-result': result'
            ] else [
                last-result': '~
            ]
        ]
        fail ~unreachable~
    ]

    'cycle combinator [
        {Run the body parser continuously in a loop until BREAK or STOP}
        return: "Result of last body parser (or none if failure)"
            [<opt> any-value!]
        parser [action!]
        <local> result' last-result' pos
    ][
        append state.loops binding of 'return

        last-result': @void

        cycle [
            ([^result' pos]: parser input) then [
                input: pos
                last-result': result'
            ] else [
                last-result': '~
            ]
        ]
        fail ~unreachable~
    ]

    'tally combinator [
        {Iterate a rule and count the number of times it matches}
        return: "Number of matches (can be 0)"
            [<opt> integer!]
        parser [action!]
        <local> count pos
    ][
        append state.loops binding of 'return

        count: 0
        cycle [
            ; !!! We discard the result, but should it be available, e.g.
            ; via a multi-return?  Can PARSE rules have multi-returns?  If
            ; so, then advanced would likely have to be done another way.  :-/
            ;
            ([^ pos]: parser input) else [
                take/last state.loops
                remainder: input
                return count
            ]
            count: count + 1
            input: pos
        ]
        fail ~unreachable~
    ]

    'break combinator [
        {Break an iterated construct like SOME or REPEAT, failing the match}
        return: "Divergent"
            []
        <local> f
    ][
        f: take/last state.loops else [
            fail "No PARSE iteration to BREAK"
        ]

        f.remainder: input
        unwind f null
    ]

    'stop combinator [
        {Break an iterated construct like SOME or REPEAT, succeeding the match}
        return: "Divergent"
            [<opt>]
        parser [<end> action!]
        <local> f result'
    ][
        result': '~  ; default `[stop]` return value as none isotope
        if :parser [  ; parser argument is optional
            ([^result' input]: parser input) else [
                return null
            ]
        ]

        f: take/last state.loops else [
            fail "No PARSE iteration to STOP"
        ]

        f.remainder: input
        unwind f unmeta result'
    ]

   === RETURN KEYWORD ===

    ; RETURN was removed for a time in Ren-C due to concerns about how it
    ; "contaminated" the return value, and that its use to avoid having to
    ; name and set a variable could lead to abruptly ending a parse before
    ; all the matching was complete.  Now UPARSE can return ANY-VALUE! and
    ; the only reason you'd ever use RETURN would be specifically for the
    ; abrupt exit...so it's fit for purpose.

    'return combinator [
        {Return a value explicitly from the parse}
        return: "Divergent"
            [<opt>]
        parser [action!]
        <local> value'
    ][
        ([^value' #]: parser input) else [
            return null
        ]

        ; !!! STATE is filled in as the frame of the top-level UPARSE call.  If
        ; we UNWIND then we bypass any of its handling code, so it won't set
        ; the /PROGRESS etc.  Review.
        ;
        unwind state unmeta value'
    ]

    === INDEX and MEASUREMENT COMBINATORS ===

    ; The historical pattern:
    ;
    ;     s: <here>, some rule, e: <here>, (len: (index of e) - (index of s))
    ;
    ; Can be done more conveniently with the <index> tag combinator:
    ;
    ;     s: <index>, some rule, e: <index>, (len: e - s)
    ;
    ; But even more conveniently with the MEASURE combinator:
    ;
    ;     len: measure some rule
    ;
    ; Note this is distinct from TALLY, which is an iterative construct that
    ; counts the number of times it can match the rule it is given:
    ;
    ;     >> parse "ababab" [tally "ab"]
    ;     == 3
    ;
    ;     >> parse "ababab" [measure some "ab"]
    ;     == 6

    <index> combinator [
        {Get the current series index of the PARSE operation}
        return: "The INDEX OF the parse position"
            [integer!]
    ][
        remainder: input
        return index of input
    ]

    'measure combinator [
        {Get the length of a matched portion of content}
        return: "Length in series units"
            [<opt> integer!]
        parser [action!]
        <local> s e
    ][
        ([^ remainder]: parser input) else [return null]

        e: index of remainder
        s: index of input

        ; Because parse operations can SEEK, this can potentially create
        ; issues.  We fail if the index is before, but could also return a
        ; bad-word! isotope.
        ;
        if s > e [
            fail "Can't MEASURE region where rules did a SEEK before the INPUT"
        ]

        return e - s
    ]

    === MUTATING KEYWORDS ===

    ; Topaz moved away from the idea that PARSE was used for doing mutations
    ; entirely.  It does complicate the implementation to be changing positions
    ; out from under things...so it should be considered carefully.
    ;
    ; UPARSE continues with the experiment, but does things a bit differently.
    ; Here CHANGE is designed to be used with value-bearing rules, and the
    ; value-bearing rule is run against the same position as the start of
    ; the input.
    ;
    ; !!! Review what happens if the input rule can modify, too.

    'change combinator [
        {Substitute a match with new data}
        return: "!!! TBD !!!"
            [<opt> bad-word!]
        parser [action!]
        replacer [action!]  ; !!! How to say result is used here?
        <local> replacement'
    ][
        ([^ remainder]: parser input) else [  ; first find end position
            return null
        ]

        ([^replacement' #]: replacer input) else [
            return null
        ]

        if bad-word? replacement' [
            fail "Cannot CHANGE to isotope"
        ]

        assert [quoted? replacement']

        ; CHANGE returns tail, use as new remainder
        ;
        remainder: change/part input (unquote replacement') remainder
        return ~changed~
    ]

    'remove combinator [
        {Remove data that matches a parse rule}
        return: "!!!TBD"
            [<opt> bad-word!]
        parser [action!]
    ][
        ([^ remainder]: parser input) else [  ; first find end position
            return null
        ]

        remainder: remove/part input remainder
        return ~removed~
    ]

    'insert combinator [
        {Insert literal data into the input series}
        return: "!!! TBD"
            [<opt> bad-word!]
        parser [action!]
        <local> insertion'
    ][
        ([^insertion' #]: parser input) else [  ; remainder ignored
            return null
        ]

        if bad-word? insertion' [
            fail "Cannot INSERT isotope"
        ]

        assert [quoted? insertion']

        remainder: insert input (unquote insertion')
        return ~inserted~
    ]

    === SEEKING KEYWORDS ===

    'to combinator [
        {Match up TO a certain rule (result position before succeeding rule)}
        return: "The rule's product"
            [<opt> any-value!]
        parser [action!]
        <local> result'
    ][
        cycle [
            ([^result' #]: parser input) then [
                remainder: input  ; TO means do not include match range
                return unmeta result'
            ]
            if tail? input [  ; could be `to <end>`, so check tail *after*
                return null
            ]
            input: next input
        ]
        fail ~unreachable~
    ]

    'thru combinator [
        {Match up THRU a certain rule (result position after succeeding rule)}
        return: "The rule's product"
            [<opt> any-value!]
        parser [action!]
        <local> result'
    ][
        cycle [
            ([^result' remainder]: parser input) then [
                return unmeta result'
            ]
            if tail? input [  ; could be `thru <end>`, check TAIL? *after*
                return null
            ]
            input: next input
        ]
        fail ~unreachable~
    ]

    'seek combinator [
        return: "seeked position"
            [any-series!]
        parser [action!]
        <local> where
    ][
        ([^where remainder]: parser input) else [
            return null
        ]
        if bad-word? where [
            fail "Cannot SEEK to isotope"
        ]
        where: my unquote
        case [
            blank? where [
                ; Allow opting out
            ]
            integer? where [
                remainder: at head input where
            ]
            any-series? :where [
                if not same? head input head where [
                    fail "Series SEEK in UPARSE must be in the same series"
                ]
                remainder: where
            ]
            fail "SEEK requires INTEGER!, series position, or BLANK!"
        ]
        return remainder
    ]

    'between combinator [
        return: "Copy of content between the left and right parsers"
            [<opt> any-series!]
        parser-left [action!]
        parser-right [action!]
        <local> start
    ][
        ([^ start]: parser-left input) else [
            return null
        ]

        let limit: start
        cycle [
            ([^ remainder]: parser-right limit) then [  ; found it
                return copy/part start limit
            ]
            if tail? limit [  ; remainder is null
                return null
            ]
            limit: next limit
        ]
        fail ~unreachable~
    ]

    === TAG! SUB-DISPATCHING COMBINATOR ===

    ; Historical PARSE matched tags literally, while UPARSE pushes to the idea
    ; that they are better leveraged as "special nouns" to avoid interfering
    ; with the user variables in wordspace.
    ;
    ; There is an overall TAG! combinator which looks in the combinator map for
    ; specific tags.  You can replace individual tag combinators or change the
    ; behavior of tags overall completely.

    tag! combinator [
        {Special noun-like keyword subdispatcher for TAG!s}
        return: "What the delegated-to tag returned"
            [<opt> any-value!]
        pending: [blank! block!]
        value [tag!]
        <local> comb
    ][
        if not comb: :(state.combinators).(value) [
            fail ["No TAG! Combinator registered for" value]
        ]

        return [# remainder pending]: comb state input
    ]

    <here> combinator [
        {Get the current parse input position, without advancing input}
        return: "parse position"
            [any-series!]
    ][
        remainder: input
        return input
    ]

    <end> combinator [
        {Only match if the input is at the end}
        return: "End position of the parse input"
            [<opt> any-series!]
    ][
        if tail? input [
            remainder: input
            return input
        ]
        remainder: null
        return null
    ]

    <input> combinator [
        {Get the original input of the PARSE operation}
        return: "parse position"
            [any-series!]
    ][
        remainder: input
        return state.input
    ]

    <any> combinator [  ; historically called "SKIP"
        {Match one series item in input, succeeding so long as it's not at END}
        return: "One atom of series input"
            [<opt> any-value!]
    ][
        if tail? input [
            return null
        ]
        remainder: next input
        return input.1
    ]

    === ACROSS (COPY?) ===

    ; Historically Rebol used COPY to mean "match across a span of rules and
    ; then copy from the first position to the tail of the match".  That could
    ; have assignments done inside, which extract some values and omit others.
    ; You could thus end up with `y: copy x: ...` and wind up with x and y
    ; being different things, which is not intuitive.

    'across combinator [
        {Copy from the current parse position through a rule}
        return: "Copied series"
            [<opt> any-series!]
        parser [action!]
    ][
        ([^ remainder]: parser input) then [
            if any-array? input [
                return as block! copy/part input remainder
            ]
            if any-string? input [
                return as text! copy/part input remainder
            ]
            return copy/part input remainder
        ]
        return null
    ]

    'copy combinator [
        {Disabled combinator, included to help guide to use ACROSS}
        return: "Divergent"
            []
    ][
        fail [
            "Transitionally (maybe permanently?) the COPY function in UPARSE"
            "is done with ACROSS:" https://forum.rebol.info/t/1595
        ]
    ]

    === SUBPARSE COMBINATOR ===

    ; Rebol2 had a INTO combinator which only took one argument: a rule to use
    ; when processing the nested input.  There was a popular proposal that
    ; INTO would take a datatype, which would help simplify a common pattern.
    ;
    ;     ahead text! into [some "a"]  ; arity-1 form
    ;     =>
    ;     into text! [some "a"]  ; arity-2 form, from proposals/Topaz
    ;
    ; The belief being that wanting to test the type you were going "INTO" was
    ; needed more often than not, and that at worst it would incentivize adding
    ; the type as a comment.  Neither R3-Alpha nor Red adopted this proposal
    ; (but Topaz did).
    ;
    ; UPARSE reframes this not to take just a datatype, but a "value-bearing
    ; rule".  This means you can use it with generated data that is not
    ; strictly resident in the series, effectively parameterizing it like
    ; PARSE itself...so it's called SUBPARSE:
    ;
    ;     parse "((aaaa)))" [subparse [between some "(" some ")"] [some "a"]]
    ;
    ; Because any value-bearing rule can be used, GROUP! rules are also legal,
    ; which lets you break the rules up for legibility (and avoids interpreting
    ; arrays as rules themselves)
    ;
    ;     parse [| | any any any | | |] [
    ;          content: between some '| some '|
    ;          subparse (content) [some 'any]
    ;     ]
    ;
    ; arity-1 INTO may still be useful as a shorthand for SUBPARSE <ANY>, but
    ; it's also a little bit obtuse when read in context.

    'subparse combinator [
        {Perform a recursion into other data with a rule}
        return: "Result of the subparser"
            [<opt> <void> any-value!]
        parser [action!]  ; !!! Easier expression of value-bearing parser?
        subparser [action!]
        <local> subseries result'
    ][
        ([^subseries remainder]: parser input) else [
            ;
            ; If the parser in the first argument can't get a value to subparse
            ; then we don't process it.
            ;
            ; !!! Review: should we allow non-value-bearing parsers that just
            ; set limits on the input?
            ;
            return null
        ]

        if bad-word? subseries [
            fail "Cannot parse INTO an isotope synthesized result"
        ]

        assert [quoted? subseries]  ; no true null unless failure

        ; We don't just unquote the literalizing quote from ^ that's on the
        ; value (which would indicate a plain series).  We dequote fully...so
        ; we can parse INTO an arbitrarily quoted series.
        ;
        ; !!! This makes sense if-and-only-if the top level UPARSE will take
        ; quoted series.  Figure out a consistent answer.
        ;
        if not any-series? subseries: my noquote [
            fail "Need ANY-SERIES! datatype for use with INTO in UPARSE"
        ]

        ; If the entirety of the item at the input array is matched by the
        ; supplied parser rule, then we advance past the item.
        ;
        any [
            null? ([^result' subseries]: subparser subseries)
            not tail? subseries
        ] then [
            return null
        ]
        return unmeta result'
    ]

    === COLLECT AND KEEP ===

    ; The COLLECT feature was first added by Red.  However, it did not use
    ; rollback across any KEEPs that happened when a parse rule failed, which
    ; makes the feature of limited use.
    ;
    ; UPARSE has a generic framework for bubbling up gathered items.  We look
    ; through that list of items here for anything marked as collect material
    ; and remove it.

    'collect combinator [
        return: "Block of collected values"
            [<opt> block!]
        pending: [blank! block!]
        parser [action!]
        <local> subpending collected
    ][
        ([^ remainder subpending]: parser input) else [
            return null
        ]

        ; subpending can be BLANK! or a block full of items that may or may
        ; not be intended for COLLECT.  Right now the logic is that all QUOTED!
        ; items are intended for collect, so extract those from the pending
        ; array.
        ;
        ; !!! More often than not a COLLECT probably is going to be getting an
        ; array of all QUOTED! items.  If so, there's probably no great reason
        ; to do a new allocation...instead the quoteds should be mutated into
        ; unquoted forms.  Punt on such optimizations for now.
        ;
        collected: collect [
            remove-each item any [subpending #] [
                if quoted? :item [keep item, true]
            ]
        ]

        pending: subpending
        return collected
    ]

    'keep combinator [
        return: "The kept value (same as input)"
            [<opt> any-value!]
        pending: [blank! block!]
        parser [action!]
        <local> result result' subpending
    ][
        ([^result' remainder subpending]: parser input) else [
            return null
        ]
        if bad-word? result' [
            fail ["Cannot KEEP a" result' "isotope"]
        ]
        if result' = the '_ [  ; result' is quoted, so they kept a blank
            pending: _
            return ~null~  ; succeed but return null isotope
        ]

        result: unquote result'

        ; Since COLLECT is considered the most common `pending` client, we
        ; reserve QUOTED! items for collected things.  This means we go ahead
        ; and flatten blocks out into quoted items.
        ;
        ; Note that if result (unquoted) was a QUOTED!, that's what they asked
        ; to collect...which means they wanted it literally.  All collect items
        ; are appended literally--consider the list to be flattened, so
        ; COLLECT itself doesn't have to worry about splicing.
        ;
        case [
            quoted? :result [  ; wanted to KEEP the unquoted thing literally
                pending: glom subpending result'  ; just one quote level
            ]
            block? :result [
                subpending: default [copy []]  ; !!! optimize empty results?
                for-each item result [
                    ;
                    ; One level of quote is to tell this APPEND we want to add
                    ; literally (that one will be removed).  The other level is
                    ; to signal that this pending item targets COLLECT.
                    ;
                    append subpending ^ ^item
                ]
                pending: subpending
            ]
            any-the-value? :result [
                pending: glom subpending ^(plain result')  ; drop @
            ]
            any-inert? :result [  ; quote doesn't matter
                pending: glom subpending ^result'  ; just one quote level
            ]
            fail "Can only KEEP values that are QUOTED! or ANY-INERT!"
        ]

        return unmeta result'
    ]

    === GATHER AND EMIT ===

    ; With gather, the idea is to do more of a "bubble-up" type of strategy
    ; for creating objects with labeled fields.  Also, the idea that PARSE
    ; itself would switch modes.
    ;
    ; !!! A particularly interesting concept that has come up is being able
    ; to "USE" or "IMPORT" an OBJECT! so its fields are local (like a WITH).
    ; This could combine with gather, e.g.
    ;
    ;     import parse [1 "hi"] [
    ;         return gather [emit x: integer!, emit y: text!]
    ;     ]
    ;     print [x "is one and" y "is {hi}"]
    ;
    ; The idea is interesting enough that it suggests being able to EMIT with
    ; no GATHER in effect, and then have the RETURN GATHER semantic.

    'gather combinator [
        return: "The gathered object"
            [<opt> object!]
        pending: [blank! block!]
        parser [action!]
        <local> obj subpending
    ][
        ([^ remainder subpending]: parser input) else [
            return null
        ]

        ; Currently we assume that all the BLOCK! items in the pending are
        ; intended for GATHER.

        obj: make object! collect [
            remove-each item any [subpending #] [
                if block? :item [keep item, true]
            ] else [
                ; should it error or fail if subpending was BLANK! ?
            ]
        ]

        pending: subpending
        return obj
    ]

    'emit combinator [
        return: "The emitted value"
            [<opt> any-value!]
        pending: [blank! block!]
        'target [set-word! set-group!]
        parser [action!]
        <local> result'
    ][
        if set-group? target [
            (match any-word! target: do as block! target) else [
                fail [
                    "GROUP! from EMIT (...): must produce an ANY-WORD!, not"
                    ^target
                ]
            ]
        ]

        ([^result' remainder pending]: parser input) else [
            return null
        ]

        ; The value is quoted (or a BAD-WORD!) because of ^ on ^(parser input).
        ; This lets us emit null fields and isotopes, since the MAKE OBJECT!
        ; will do an evaluation.
        ;
        pending: glom pending ^ :[as set-word! target result']
        return unmeta result'
    ]

    === SET-WORD! and SET-TUPLE! COMBINATOR ===

    ; The concept behind Ren-C's SET-WORD! in PARSE is that parse combinators
    ; don't just update the remainder of the parse input, but they also return
    ; values.  If these appear to the right of a set-word, then the set word
    ; will be assigned on a match.
    ;
    ; !!! SET-PATH! is not supported, as paths will be used for functions only.

    set-word! combinator [
        return: "The set value"
            [<opt> any-value!]
        value [set-word!]
        parser "Failed parser will means target SET-WORD! will be unchanged"
            [action!]
        <local> result'
    ][
        ([^result' remainder]: parser input) else [return null]

        return set value unmeta result'
    ]

    set-tuple! combinator [
        return: "The set value"
            [<opt> any-value!]
        value [set-tuple!]
        parser "Failed parser will means target SET-TUPLE! will be unchanged"
            [action!]
        <local> result'
    ][
        ([^result' remainder]: parser input) else [return null]

        return set value unmeta result'
    ]

    'set combinator [
        {Disabled combinator, included to help guide to use SET-WORD!}
        return: "Divergent"
            []
    ][
        fail [
            "The SET keyword in UPARSE is done with SET-WORD!, and if SET does"
            "come back it would be done differently:"
            https://forum.rebol.info/t/1139
        ]
    ]

    set-group! combinator [
        return: "The set value"
            [<opt> any-value!]
        value [set-group!]
        parser "Failed parser will means target will be unchanged"
            [action!]
        <local> result'
    ][
        let var: eval value
        ([^result' remainder]: parser input) else [return null]
        return set var unmeta result'
    ]

    === TEXT! COMBINATOR ===

    ; For now we just make text act as FIND/MATCH/TAIL, though this needs to be
    ; sensitive to whether we are operating on blocks or text/binary.
    ;
    ; !!! We presume that it's value-bearing, and gives back the value it
    ; matched against.  If you don't want it, you have to ELIDE it.  Note this
    ; value is the rule in the string and binary case, but the item in the
    ; data in the block case.

    text! combinator [
        return: "The rule series matched against (not input value)"
            [<opt> text!]
        value [text!]
    ][
        case [
            any-array? input [
                if input.1 <> value [
                    return null
                ]
                remainder: next input
                return input.1
            ]

            ; for both of these cases, we have to use the rule, since there's
            ; no isolated value to capture.  Should we copy it?

            any-string? input [
                input: apply :find [
                    input value
                    /match true
                    /tail true
                    /case state.case
                ] else [
                    return null
                ]
            ]
            true [
                assert [binary? input]
                input: apply :find [
                    input value
                    /match true
                    /tail true
                    /case state.case
                ] else [
                    return null
                ]
            ]
        ]

        remainder: input
        return value
    ]

    === TOKEN! COMBINATOR (currently ISSUE! and CHAR!) ===

    ; The TOKEN! type is an optimized immutable form of string that will
    ; often be able to fit into a cell with no series allocation.  This makes
    ; it good for representing characters, but it can also represent short
    ; strings.  It matches case-sensitively.

    issue! combinator [
        return: "The token matched against (not input value)"
            [<opt> issue!]
        value [issue!]
    ][
        case [
            any-array? input [
                if input.1 == value [
                    remainder: next input
                    return input.1
                ]
                return null
            ]
            any-string? input [
                if remainder: find/match/case/tail input value [
                    return value
                ]
                return null
            ]
            true [
                assert [binary? input]
                if remainder: find/match/case/tail input value [
                    return value
                ]
                return null
            ]
        ]
    ]

    === BINARY! COMBINATOR ===

    ; Arbitrary matching of binary against text is a bit of a can of worms,
    ; because if we AS alias it then that would constrain the binary...which
    ; may not be desirable.  Also you could match partial characters and
    ; then not be able to set a string position.  So we don't do that.

    binary! combinator [
        return: "The binary matched against (not input value)"
            [<opt> binary!]
        value [binary!]
    ][
        case [
            any-array? input [
                if input.1 = value [
                    remainder: next input
                    return input.1
                ]
                return null
            ]
            true [  ; Note: BITSET! acts as "byteset" here
                ; binary or any-string input
                if remainder: find/match/tail input value [
                    return value
                ]
                return null
            ]
        ]
    ]

    === GROUP! AND PHASE COMBINATOR ===

    ; GROUP! does not advance the input, just runs the group.  It can return
    ; a value, which is used by value-accepting combinators.  Use ELIDE if
    ; this would be disruptive in terms of a value-bearing BLOCK! rule.
    ;
    ; There is a new and important feature where a GROUP! can ask to be
    ; deferred until a PHASE is complete (where the parse as a whole also
    ; counts as a phase).  This is done using the <delay> tag (may not be
    ; the best name).

    group! combinator [
        return: "Result of evaluating the group (invisible if <delay>)"
            [<void> <opt> any-value!]
        pending: [blank! block!]
        value [any-array!]  ; allow any array to use this "DO combinator"
    ][
        remainder: input

        if <delay> = first value [
            if length of value = 1 [
                fail "Use ('<delay>) to evaluate to the tag <delay> in GROUP!"
            ]
            pending: :[next value]  ; GROUP! signals delayed groups
            return void
        ]

        ; If a GROUP! evaluates to NULL, we want the overall rule to evaluate
        ; to ~null~ isotope...because for instance `x: (null)` should set
        ; x to NULL and not fail the SET-WORD! combinator rule...which is
        ; what true NULL would signify.  Use HEAVY.
        ;
        pending: _
        return heavy eval value
    ]

    'phase combinator [
        return: "Result of the parser evaluation"
            [<void> <opt> any-value!]
        pending: [blank! block!]
        parser [action!]
        <local> subpending result'
    ][
        ([^result' remainder subpending]: parser input) else [
            pending: _
            return null
        ]

        ; Run GROUP!s in order, removing them as one goes
        ;
        remove-each item any [subpending #] [
            if group? :item [eval item, true]
        ]

        pending: subpending
        return unmeta result'
    ]

    === GET-GROUP! COMBINATOR ===

    ; This allows the running of a rule generated by code, as if it had been
    ; written in the spot where the GET-GROUP! is.
    ;
    ; !!! The original GET-GROUP! concept allowed:
    ;
    ;     parse "aaa" [:('some) "a"]
    ;
    ; To do that would require some kind of back-channel of communication
    ; to the BLOCK! combinator to return the material, or have specific code
    ; in the BLOCK! combinator for processing GET-GROUP!.  Having it as a
    ; combinator the caller of UPARSE can redefine or override in the
    ; /COMBINATORS map is much more compelling, so punt on that for now.

    get-group! combinator [
        return: "Result of running combinator from fetching the WORD!"
            [<opt> <void> any-value!]
        pending: [blank! block!]   ; we retrigger combinator; it may KEEP, etc.

        value [any-array!]  ; allow any array to use this "REPARSE-COMBINATOR"
        <local> r comb
    ][
        ; !!! The rules of what are allowed or not when triggering through
        ; WORD!s likely apply here.  Should it all be repeated?

        r: (^ eval value) else [
            fail "GET-GROUP! evaluated to NULL"  ; no NULL rules, mistake?
        ]

        if r = @void [  ; like [:(if false [...])] or [:(comment "hi")]
            pending: _
            remainder: input
            return void
        ]

        if bad-word? r [
            if r = '~null~ [
                fail "GET-GROUP! evaluated to ~NULL~ isotope"  ; also mistake?
            ]

            fail ["Bad isotope from GET-GROUP!" r]  ; fail all other isotopes
        ]

        r: unquote r

        if integer? :r [
            fail [value "can't be INTEGER!, use REPEAT" :["(" value ")"]]
        ]
        if word? :r [
            fail [value "can't be WORD!, use" :["[" value "]"] "BLOCK! rule"]
        ]

        if not comb: select state.combinators kind of :r [
            fail ["Unhandled type in GET-GROUP! combinator:" kind of :r]
        ]

        ; !!! We don't need to call COMBINATORIZE because we can't handle
        ; arguments here, but this should have better errors if the datatype
        ; combinator takes arguments.
        ;
        return [# remainder pending]: comb state input :r
    ]

    === GET-BLOCK! COMBINATOR ===

    ; If the GET-BLOCK! combinator were going to have a meaning, it would likely
    ; have to be "run this block as a rule, and use the synthesized product
    ; as a rule"
    ;
    ;     >> did parse "aaabbb" [:[some "a" ([some "b"])]
    ;     == #[true]
    ;
    ; It's hard offhand to think of great uses for that, but that isn't to say
    ; that they don't exist.

    get-block! combinator [
        return: "Undefined at this time"
            [<opt> any-value!]
        value [get-block!]
    ][
        fail "No current meaning for GET-BLOCK! combinator"
    ]

    === BITSET! COMBINATOR ===

    ; There is some question here about whether a bitset used with a BINARY!
    ; can be used to match UTF-8 characters, or only bytes.  This may suggest
    ; a sort of "INTO" switch that could change the way the input is being
    ; viewed, e.g. being able to do INTO BINARY! on a TEXT! (?)

    bitset! combinator [
        return: "The matched input value"
            [<opt> char! integer!]
        value [bitset!]
    ][
        case [
            any-array? input [
                if input.1 = value [
                    remainder: next input
                    return input.1
                ]
            ]
            any-string? input [
                if find value try input.1 [
                    remainder: next input
                    return input.1
                ]
            ]
            true [
                assert [binary? input]
                if find value try input.1 [
                    remainder: next input
                    return input.1
                ]
            ]
        ]
        return null
    ]

    === QUOTED! COMBINATOR ===

    ; When used with ANY-ARRAY! input, recognizes values literally.  When used
    ; with ANY-STRING! it will convert the value to a string before matching.

    quoted! combinator [
        return: "The matched value"
            [<opt> any-value!]
        pending: [blank! block!]
        value [quoted!]
        <local> comb
    ][
        ; Review: should it be legal to say:
        ;
        ;     >> parse "" [' (1020)]
        ;     == 1020
        ;
        ; Arguably there is a null match at every position.  A ^null might
        ; also be chosen to match)...but NULL rules are errors.
        ;
        if any-array? input [
            if :input.1 = unquote value [
                remainder: next input
                pending: _
                return unquote value
            ]
            return null
        ]

        if any-string? input [
            ;
            ; Not exactly sure what this operation is, but it's becoming more
            ; relevant...it's like FORM by putting < > on tags (TO TEXT! won't)
            ; and it will merge strings in blocks without spacing.
            ;
            value: append copy {} unquote value

            comb: :(state.combinators).(text!)
            return [# remainder pending]: comb state input value
        ]

        assert [binary? input]

        value: to binary! unquote value
        comb: :(state.combinators).(binary!)
        return [# remainder pending]: comb state input value
    ]

    'lit combinator [  ; should long form be LITERALLY or LITERAL ?
        return: "Literal value" [<opt> any-value!]
        pending: [blank! block!]
        'value [any-value!]
        <local> comb
    ][
        ; Though generic quoting exists, being able to say [lit ''x] instead
        ; of ['''x] may be clarifying when trying to match ''x (for instance)

        comb: :(state.combinators).(quoted!)
        return [# remainder pending]: comb state input ^value
    ]

    === NON-ADVANCING OPERATORS ===

    ; These all act as their normal forms, to save you from the uglier THE/
    ; or META/ invocations.

    'the combinator [
        return: "Quoted form of literal value (not matched)" [any-value!]
        'value [any-value!]
    ][
        remainder: input
        return :value
    ]

    'just combinator [
        return: "Quoted form of literal value (not matched)" [quoted!]
        'value [any-value!]
    ][
        remainder: input
        return quote :value
    ]

    === BLANK! COMBINATOR ===

    ; Follows general philosophy of No-Op with BLANK-IN-NULL-out

    blank! combinator [
        return: "NULL (in isotope form)"
            [<opt>]
        value [blank!]
    ][
        remainder: input
        pending: _
        return ~null~  ; combinators RETURN/NO-DECAY by default
    ]

    === LOGIC! COMBINATOR ===

    ; Handling of LOGIC! in Ren-C replaces the idea of FAIL, because a logic
    ; #[true] is treated as "continue parsing" while #[false] is "rule did
    ; not match".  When combined with GET-GROUP!, this fully replaces the
    ; need for the IF construct.
    ;
    ; e.g. parse "..." [:(mode = 'read) ... | :(mode = 'write) ...]

    logic! combinator [
        return: "True if success, null if failure"
            [<opt> logic!]
        value [logic!]
    ][
        if value [
            remainder: input
            return true
        ]
        return null
    ]

    === INTEGER! COMBINATOR ===

    ; The behavior of INTEGER! in UPARSE is to just evaluate to itself.  If you
    ; want to repeat a rule a certain number of times, you have to say REPEAT.
    ;
    ; A compatibility combinator for Rebol2 PARSE has the repeat semantics.
    ;
    ; It is possible to break this regularity with quoted/skippable combinator
    ; arguments.  And it's necessary to do so for the Redbol emulation.  See
    ; the INTEGER! combinator used in PARSE2 for this "dirty" technique.
    ;
    ; Note that REPEAT allows the use of BLANK! to opt out of an iteration.

    integer! combinator [
        return: "Just the INTEGER! (see REPEAT for repeating rules)"
            [integer!]
        value [integer!]
    ][
        remainder: input
        return value
    ]

    'repeat combinator [
        return: "Last parser result"
            [<opt> <void> any-value!]
        times-parser [action!]
        parser [action!]
        <local> times' min max result' i temp-remainder
    ][
        ([^times' input]: times-parser input) else [return null]

        switch type of unmeta times' [
            blank! [
                remainder: input
                return void  ; `[repeat (_) rule]` is a no-op
            ]
            issue! [
                if times' <> the '# [
                    fail ["REPEAT takes ISSUE! of # to act like MAYBE SOME"]
                ]
                min: 0, max: #
            ]
            integer! [
                max: min: unquote times'
            ]
            block! the-block! [
                parse unquote times' [
                    '_ '_ <end> (
                        remainder: input
                        return void  ; `[repeat ([_ _]) rule]` is a no-op
                    )
                    |
                    min: [integer! | '_ (0)], max: [integer! | '_ | '#]
                ]
                max: default [min]  ; max being blank implies `max = min`
            ]
        ] else [
            fail "REPEAT combinator requires INTEGER! or [INTEGER! INTEGER!]"
        ]

        all [max <> #, max < min] then [
            fail "Can't make MAX less than MIN in range for REPEAT combinator"
        ]

        append state.loops binding of 'return

        result': @void  ; `repeat (0) <any>` => void intent

        count-up i max [  ; will count infinitely if max is #
            ;
            ; After the minimum number of repeats are fulfilled, the parser
            ; may not match and return the last successful result.  So
            ; we don't do the assignment in the `[...]:` multi-return in case
            ; it would overwrite the last useful result.  Instead, the GROUP!
            ; potentially returns...only do the assignment if it does not.
            ;
            result': (
                ([^ temp-remainder]: parser input) also [
                    input: temp-remainder  ; only overwrite input on success
                ] else [
                    take/last state.loops
                    if i <= min [  ; `<=` not `<` as this iteration failed!
                        return null
                    ]
                    remainder: input
                    return unmeta result'
                ]
            )
        ]

        take/last state.loops
        remainder: input
        return unmeta result'
    ]

    === DATATYPE! COMBINATOR ===

    ; Traditionally you could only use a datatype with ANY-ARRAY! types,
    ; but since Ren-C uses UTF-8 Everywhere it makes it practical to merge in
    ; transcoding:
    ;
    ;     >> parse "{Neat!} 1020" [t: text! i: integer!]
    ;     == "{Neat!} 1020"
    ;
    ;     >> t
    ;     == "Neat!"
    ;
    ;     >> i
    ;     == 1020
    ;
    ; !!! TYPESET! is on somewhat shaky ground as "a thing", so it has to
    ; be thought about as to how `s: any-series!` or `v: any-value!` might
    ; work.  It could be that there's a generic TRANSCODE operation and
    ; then you can filter the result of that.

    datatype! combinator [
        return: "Matched or synthesized value"
            [<opt> any-value!]
        value [datatype!]
        <local> item error
    ][
        either any-array? input [
            if value <> kind of input.1 [
                return null
            ]
            remainder: next input
            return input.1
        ][
            any [
                ; !!! Not all errors are recoverable in transcode, so some
                ; actually fail vs. return an error, e.g.:
                ;
                ;     parse to binary! "#(" [blank!]
                ;
                ; So we actually need this TRAP here.  Review.
                ;
                trap [
                    [item remainder @error]: transcode input
                ]
                value != type of :item
            ] then [
                return null
            ]
            return :item
        ]
    ]

    typeset! combinator [
        return: "Matched or synthesized value"
            [<opt> any-value!]
        value [typeset!]
        <local> item error
    ][
        either any-array? input [
            if not find value try (kind of input.1) [
                return null
            ]
            remainder: next input
            return input.1
        ][
            any [
                [item remainder error]: transcode input
                not find value (type of :item)
            ] then [
                return null
            ]
            return :item
        ]
    ]

    === THE-XXX! COMBINATORS ===

    ; What the THE-XXX! combinators are for is doing literal matches vs. a
    ; match through a rule, e.g.:
    ;
    ;     >> block: [some "a"]
    ;
    ;     >> parse [[some "a"] [some "a"]] [some @block]
    ;     == [some "a"]
    ;
    ; However they need to have logic in them to do things like partial string
    ; matches on text.  That logic has to be in the QUOTED! combinator, so
    ; this code builds on that instead of repeating it.
    ;
    ; !!! These follow a simple pattern, could generate at a higher level.

    '@ combinator [
        return: "Match product of result of applying rule" [<opt> any-value!]
        pending: [blank! block!]
        parser [action!]
        <local> comb result'
    ][
        ([^result' remainder]: parser input) else [return null]

        comb: :(state.combinators).(quoted!)
        return [# remainder pending]: comb state remainder result'
    ]

    the-word! combinator [
        return: "Literal value" [<opt> any-value!]
        pending: [blank! block!]
        value [the-word!]
        <local> comb
    ][
        comb: :(state.combinators).(quoted!)
        return [# remainder pending]: comb state input quote get value
    ]

    the-path! combinator [
        return: "Literal value" [<opt> any-value!]
        pending: [blank! block!]
        value [the-word!]
        <local> comb
    ][
        comb: :(state.combinators).(quoted!)
        return [# remainder pending]: comb state input quote get value
    ]

    the-tuple! combinator [
        return: "Literal value" [<opt> any-value!]
        pending: [blank! block!]
        value [the-tuple!]
        <local> comb
    ][
        comb: :(state.combinators).(quoted!)
        return [# remainder pending]: comb state input quote get value
    ]

    the-group! combinator [
        return: "Literal value" [<opt> any-value!]
        pending: [blank! block!]
        value [the-group!]
        <local> result' comb totalpending single
    ][
        totalpending: copy []
        value: as group! value
        comb: :(state.combinators).(group!)
        ([^result' remainder pending]: comb state input value) else [
            return null
        ]
        totalpending: glom totalpending pending

        comb: :(state.combinators).(quoted!)

        all [
            length of value = 1
            group? single: first value
            length of single = 1
        ] then [
            ; Splice-match semantics for @((thing)), match the block itemwise.
            ; Experimental feature with weird syntax...
            ;
            if (not quoted? result') or (not block? unmeta result') [
                fail "Inline matching @((...)) requires BLOCK! for the moment"
            ]
            for-each item unmeta result' [
                ([^result' remainder pending]: comb state remainder quote item) else [
                    return null
                ]
                totalpending: glom totalpending pending
            ]
        ] else [
            ([^result' remainder pending]: comb state remainder result') else [
                return null
            ]
            totalpending: glom totalpending pending
        ]
        pending: totalpending
        return unmeta result'
    ]

    the-block! combinator [
        return: "Literal value" [<opt> any-value!]
        pending: [blank! block!]
        value [the-block!]
        <local> result' comb totalpending
    ][
        ; !!! THE-BLOCK! acting as just matching the block is redundant with
        ; a quoted block.  Suggestions have been to repurpose @[...] for a
        ; datatype representation, but that's inconsistent with other uses.
        ;
        ; So the most "logical" thing to do here is to say it means "literally
        ; match the result of running the block as a rule".  This is not quite
        ; as useless as it sounds, since a rule can synthesize an arbitrary
        ; value.

        totalpending: copy []
        value: as block! value
        comb: :(state.combinators).(block!)
        ([^result' input pending]: comb state input value) else [
            return null
        ]
        totalpending: glom totalpending pending
        comb: :(state.combinators).(quoted!)
        ([^result' remainder pending]: comb state input result') else [
            return null
        ]
        totalpending: glom totalpending pending
        pending: totalpending
        return unmeta result'
    ]

    === META-XXX! COMBINATORS ===

    ; The META-XXX! combinators add a quoting level to their result, with
    ; some quirks surrounding nulls and BAD-WORD! isotopes.  The quoting is
    ; important with functions like KEEP...but advanced tunneling of behavior
    ; regarding unsets, nulls, and invisibility requires the feature.
    ;
    ; Note: These are NOT fixed as `[:block-combinator | :meta]`, because
    ; they want to inherit whatever combinator that is currently in use for
    ; their un-meta'd type (by default).  This means dynamically reacting to
    ; the set of combinators chosen for the particular parse.
    ;
    ; !!! These follow a simple pattern, could all use the same combinator and
    ; just be sensitive to the received type of value.

    '^ combinator [
        return: "Meta quoted" [<opt> bad-word! quoted! the-word!]
        parser [action!]
    ][
        return [^ remainder]: parser input
    ]

    meta-word! combinator [
        return: "Meta quoted" [<opt> bad-word! quoted! the-word!]
        pending: [blank! block!]
        value [meta-word!]
        <local> comb
    ][
        value: as word! value
        comb: :(state.combinators).(word!)
        return [^ remainder pending]: comb state input value  ; leave meta
    ]

    meta-tuple! combinator [
        return: "Meta quoted" [<opt> bad-word! quoted! the-word!]
        pending: [blank! block!]
        value [meta-tuple!]
        <local> comb
    ][
        value: as tuple! value
        comb: :(state.combinators).(tuple!)
        return [^ remainder pending]: comb state input value  ; leave meta
    ]

    meta-path! combinator [
        return: "Meta quoted" [<opt> bad-word! quoted! the-word!]
        pending: [blank! block!]
        value [meta-path!]
        <local> comb
    ][
        value: as path! value
        comb: :(state.combinators).(path!)
        return [^ remainder pending]: comb state input value  ; leave meta
    ]

    meta-group! combinator [
        return: "Meta quoted" [<opt> bad-word! quoted! the-word!]
        pending: [blank! block!]
        value [meta-group!]
        <local> comb
    ][
        value: as group! value
        comb: :(state.combinators).(group!)
        return [^ remainder pending]: comb state input value  ; leave meta
    ]

    meta-block! combinator [
        return: "Meta quoted" [<opt> bad-word! quoted! the-word!]
        pending: [blank! block!]
        value [meta-block!]
        <local> comb
    ][
        value: as block! value
        comb: :(state.combinators).(block!)
        return [^ remainder pending]: comb state input value  ; leave meta
    ]

    === INVISIBLE COMBINATORS ===

    ; If BLOCK! is asked for a result, it will accumulate results from any
    ; result-bearing rules it hits as it goes.  Not all rules give results
    ; by default--such as GROUP! or literals for instance.  If something
    ; gives a result and you do not want it to, use ELIDE.
    ;
    ; !!! Suggestion has made that ELIDE actually be SKIP.  This sounds good,
    ; but would require SKIP as "match next item" having another name.

    'elide combinator [
        {Transform a result-bearing combinator into one that has no result}
        return: "Invisible"
            [<void> <opt>]
        parser [action!]
    ][
        ([^ remainder]: parser input) else [
            return null
        ]
        return void
    ]

    'comment combinator [
        {Comment out an arbitrary amount of PARSE material}
        return: "Invisible)"
            [<void>]
        'ignored [block! text! tag! issue!]
    ][
        ; !!! This presents a dilemma, should it be quoting out a rule, or
        ; quoting out material that's quoted?  This *could* make a parser and
        ; simply not call it:
        ;
        ;    >> parse "aaa" [3 "a" comment across some "b"]
        ;    == "a"
        ;
        ; In any case, forming a parser rule that's not going to be run is
        ; less efficient than just quoting material, which can be done on
        ; rules with illegal content:
        ;
        ;    >> parse "a" [comment [across some "a" ~illegal~] "a"]
        ;    == "a"
        ;
        ; The more efficient and flexible form is presumed here to be the
        ; most useful, and the closest parallel to the plain COMMENT action.
        ;
        remainder: input
        return void
    ]

    'skip combinator [
        {Historical SKIP is deprecated, give an error}
        return: "Invisible" [<opt> <void>]
        parser [action!]
        <local> result
    ][
        ([result #]: parser input) else [return null]

        if blank? :result [
            remainder: input
            return void
        ]
        if not integer? :result [
            fail "SKIP expects INTEGER! amount to skip"
        ]
        remainder: skip input result else [
            return null  ; out of range SKIP gives NULL, not considered match
        ]
        return void
    ]

    === ACTION! COMBINATOR ===

    ; The ACTION! combinator is a new idea of letting you call a normal
    ; function with parsers fulfilling the arguments.  At the Montreal
    ; conference Carl said he was skeptical of PARSE getting any harder to read
    ; than it was already, so the isolation of DO code to GROUP!s seemed like
    ; it was a good idea, but maybe allowing calling zero-arity functions.
    ;
    ; With Ren-C it's easier to try these ideas out.  So the concept is that
    ; you can make a PATH! that ends in / and that will be run as a normal
    ; ACTION!, but whose arguments are fulfilled via PARSE.

    action! combinator [
        {Run an ordinary ACTION! with parse rule products as its arguments}
        return: "The return value of the action"
            [<opt> <void> any-value!]
        pending: [blank! block!]
        value [action!]
        ; AUGMENT is used to add param1, param2, param3, etc.
        /parsers "Sneaky argument of parsers collected from arguments"
            [block!]
        <local> arg totalpending
    ][
        ; !!! We very inelegantly pass a block of PARSERS for the argument in
        ; because we can't reach out to the augmented frame (rule of the
        ; design) so the augmented function has to collect them into a block
        ; and pass them in a refinement we know about.  This is the beginning
        ; of a possible generic mechanism for variadic combinators, but it's
        ; tricky because the variadic step is before this function actually
        ; runs...review as the prototype evolves.

        ; !!! We cannot use the autopipe mechanism because the hooked combinator
        ; does not see the augmented frame.  Have to do it manually.
        ;
        totalpending: _

        let f: make frame! :value
        for-each param (parameters of action of f) [
            if not path? param [
                ensure action! :parsers.1
                if meta-word? param [
                    param: to word! param
                    f.(param): ([^ input pending]: parsers.1 input) else [
                        return null
                    ]
                ] else [
                    f.(param): ([# input pending]: parsers.1 input) else [
                        return null
                    ]
                ]
                totalpending: glom totalpending pending
                parsers: next parsers
            ]
        ]
        assert [tail? parsers]
        remainder: input
        pending: totalpending
        return eval f
    ]

    === WORD! and TUPLE! COMBINATOR ===

    ; The WORD! combinator handles *non-keyword* WORD! dispatches.  It cannot
    ; also handle keywords, because that would mean it would have to be able
    ; to take the parameters to those keywords...hence needing to advance the
    ; rules--or be a "variadic" combinator.
    ;
    ; The operation is basically the same for TUPLE!, so the same code is used.

    word! combinator [
        return: "Result of running combinator from fetching the WORD!"
            [<opt> <void> any-value!]
        pending: [blank! block!]
        value [word! tuple!]
        <local> r comb
    ][
        r: case [
            ; !!! The CHAR!=ISSUE! => TOKEN! change has not really been fully
            ; finished, which is bad.  Work around it by making CHAR! a
            ; synonym for ISSUE!, instead of the "type constraint" META-WORD!
            ;
            value = 'char! [issue!]
            true [get value]
        ]
        *else [
            fail "WORD! fetches cannot be NULL in UPARSE (use BLANK!)"
        ]

        ; !!! It's not clear exactly what set of things should be allowed or
        ; disallowed here.  Letting you do INTEGER! has been rejected as too
        ; obfuscating, since the INTEGER! combinator takes an argument.  Letting
        ; Allowing WORD! to run the WORD! combinator again would not be letting
        ; you do anything with recursion you couldn't do with BLOCK! rules, but
        ; still seems kind of bad.  Allowing LOGIC! seems like it may be
        ; confusing but if it weren't allowed you couldn't break rules just
        ; by using the word FALSE.
        ;
        ; Basically, for the moment, we rule out anything that takes arguments.
        ; Right now that's integers and keywords, so just prohibit those.
        ;
        if integer? :r [
            fail [value "can't be INTEGER!, use REPEAT" :["(" value ")"]]
        ]
        if word? :r [
            fail [value "can't be WORD!, use" :["[" value "]"] "BLOCK! rule"]
        ]

        ; !!! Type constraints like "quoted word" or "quoted block" have not
        ; been sorted out, unfortunately.  But LIT-WORD! being matched was
        ; something historically taken for granted.  So LIT-WORD! is the fake
        ; "pseudotype" of ^lit-word! at the moment.  Embrace it for the moment.
        ;
        if :r = lit-word! [
            if not any-array? input [
                fail "LIT-WORD! hack only works with array inputs"
            ]
            all [quoted? input.1, word? unquote input.1] then [
                pending: _
                remainder: next input
                return unquote input.1
            ] else [
                return null
            ]
        ]
        if :r = lit-path! [
            if not any-array? input [
                fail "LIT-PATH! hack only works with array inputs"
            ]
            all [quoted? input.1, path? unquote input.1] then [
                pending: _
                remainder: next input
                return unquote input.1
            ] else [
                return null
            ]
        ]

        if not comb: select state.combinators kind of :r [
            fail ["Unhandled type in WORD! combinator:" kind of :r]
        ]

        ; !!! We don't need to call COMBINATORIZE because we can't handle
        ; arguments here, but this should have better errors if the datatype
        ; combinator takes arguments.
        ;
        return [# remainder pending]: comb state input :r
    ]

    === NEW-STYLE ANY COMBINATOR ===

    ; Historically ANY was a synonym for what is today MAYBE SOME FURTHER.  It
    ; was seen as a confusing usage of the word ANY given its typical meaning
    ; related to picking a single item from a list of alternatives--not a
    ; looping construct.
    ;
    ; The new meaning of ANY provides the ability to treat a BLOCK! received
    ; as an argument as a block of alternatives.  This can frequently be more
    ; convenient than trying to build a block of alternatives with `|`, as
    ; that has conditions on the edges where [| ...] is always a no-op as it
    ; means the same thing as [[] | ...], and [... |] will always succeed if
    ; the other alternatives fail because it is the same as [... | []].
    ;
    ; Due to not wanting to handle the block argument as a rule dispatched to
    ; the block combinator, syntax is `any (alternates)` not `any alternates`.

    'any combinator [
        return: "Last result value"
            [<opt> <void> any-value!]
        pending: [blank! block!]
        'arg "To catch instances of old ANY, only GROUP! and THE-BLOCK!"
            [any-value!]  ; lie and take ANY-VALUE! to report better error
        <local> result' block
    ][
        switch type of :arg [
            group! [
                if not block? block: eval arg [
                    fail ["The ANY combinator requires a BLOCK! of alternates"]
                ]
            ]
            block! [block: arg]
        ] else [
            fail [
                "The ANY combinator in UPARSE is not an iterating construct."
                "Use MAYBE SOME, OPT SOME FURTHER, etc. depending on purpose:"
                https://forum.rebol.info/t/1572
            ]
        ]

        while [not tail? block] [
            ;
            ; Turn next alternative into a parser ACTION!, and run it.
            ; We take the first parser that succeeds.
            ;
            let [parser 'block]: parsify state block
            ([^result' remainder pending]: parser input) then [
                return unmeta result'
            ]
        ]

        return null  ; they all failed, so the ANY failed
    ]

    === BLOCK! COMBINATOR ===

    ; Handling of BLOCK! is the central combinator.  The contents are processed
    ; as a set of alternatives separated by `|`, with a higher-level sequence
    ; operation indicated by `||`.  The bars are treated specially; e.g. | is
    ; not an "OR combinator" due to semantics, because if arguments to all the
    ; steps were captured in one giant ACTION! that would mean changes to
    ; variables by a GROUP! would not be percieved by later steps...since once
    ; a rule like SOME captures a variable it won't see changes:
    ;
    ; https://forum.rebol.info/t/when-should-parse-notice-changes/1528
    ;
    ; (There is also a performance benefit, since if | was a combinator then
    ; a sequence of 100 alternates would have to build a very large OR
    ; function...rather than being able to build a small function for each
    ; step that could short circuit before the others were needed.)

    block! combinator [
        return: "Last result value"
            [<opt> <void> any-value!]
        pending: [blank! block!]
        value [block!]
        /limit "Limit of how far to consider (used by <...> recursion)"
            [block!]
        /thru "Keep trying rule until end of block"
        <local> rules pos result' f sublimit totalpending subpending
    ][
        rules: value  ; alias for clarity
        limit: default [tail of rules]
        pos: input

        totalpending: _  ; can become GLOM'd into a BLOCK!

        result': @void  ; default result is void

        while [not same? rules limit] [
            if state.verbose [
                print ["RULE:" mold/limit rules 60]
                print ["INPUT:" mold/limit pos 60]
                print "---"
            ]

            if rules.1 = ', [  ; COMMA! is only legal between steps
                rules: my next
                continue
            ]

            if rules.1 = '| [
                ;
                ; Rule alternative was fulfilled.  Base case is a match, e.g.
                ; with input "cde" then [| "ab"] will consider itself to be a
                ; match before any input is consumed, e.g. before the "c".
                ;
                ; But UPARSE has an extra trick up its sleeve with `||`, so
                ; you can have a sequence of alternates within the same block.
                ; scan ahead to see if that's the case.
                ;
                ; !!! This carries a performance penalty, as successful matches
                ; must scan ahead through the whole rule of blocks just as a
                ; failing match would when searching alternates.  Caching
                ; "are there alternates beyond this point" or "are there
                ; sequences beyond this point" could speed that up as flags on
                ; cells if they were available to the internal implementation.
                ;
                catch [  ; use CATCH to continue outer loop
                    let r
                    while [r: rules.1] [
                        rules: my next
                        if r = '|| [
                            input: pos  ; don't roll back past current pos
                            throw <inline-sequence-operator>
                        ]
                    ]
                ] then [
                    continue
                ]

                ; If we didn't find an inline sequencing operator, then the
                ; successful alternate means the whole block is done.
                ;
                remainder: pos
                pending: totalpending
                return unmeta result'
            ]

            ; If you hit an inline sequencing operator here then it's the last
            ; alternate in a list.
            ;
            if rules.1 = '|| [
                input: pos  ; don't roll back past current pos
                rules: my next
                continue
            ]

            ; Do one "Parse Step".  This involves turning whatever is at the
            ; next parse position into an ACTION!, then running it.
            ;
            if rules.1 = '<...> [  ; "variadic" parser, use recursion
                rules: next rules
                if tail? rules [  ; if at end, act like [elide to <end>]
                    remainder: tail of pos
                    pending: totalpending
                    return unmeta result'
                ]
                sublimit: find/part rules <...> limit

                f: make frame! action of binding of 'return  ; this combinator
                f.state: state
                f.value: rules
                f.limit: sublimit
                f.thru: #

                rules: sublimit else [tail of rules]
            ] else [
                f: make frame! [# rules]: parsify state rules
            ]

            f.input: pos
            f.remainder: 'pos
            f.pending: 'subpending

            ; Note: This can't be `eval f then ^temp -> [...]` because the
            ; maybe expression can vanish.
            ;
            (^ eval f) then temp -> [
                if error? temp [  ; !!! Rethink when right moment for errors is
                    fail temp
                ]
                if temp <> @void  [
                    result': temp  ; overwrite if was visible
                ]
                totalpending: glom totalpending subpending
            ] else [
                result': @void  ; reset, e.g. `[false |]`

                free totalpending  ; proactively release memory
                totalpending: _

                ; If we fail a match, we skip ahead to the next alternate rule
                ; by looking for an `|`, resetting the input position to where
                ; it was when we started.  If there are no more `|` then all
                ; the alternates failed, so return NULL.
                ;
                pos: catch [
                    let r
                    while [r: rules.1] [
                        rules: my next
                        if r = '| [throw input]  ; reset POS

                        ; If we see a sequencing operator after a failed
                        ; alternate, it means we can't consider the alternates
                        ; across that sequencing operator as candidates.  So
                        ; return null just like we would if reaching the end.
                        ;
                        if r = '|| [break]
                    ]
                ] else [
                    if (not thru) or (tail? input) [
                        remainder: null
                        return null
                    ]
                    rules: value
                    pos: input: my next
                    continue
                ]
            ]
        ]

        remainder: pos
        pending: totalpending
        return unmeta result'
    ]

    === FAIL COMBINATOR ===

    ; Gracefully handling failure has two places that you might want to
    ; provide assistance in implicating...one is the parse rules, and the other
    ; is the parse input.
    ;
    ; The FAIL combinator is--perhaps obviously (?)--not for pointing out
    ; syntax errors in the rules.  Because it's a rule.  So by default it will
    ; complain about the location where you are in the input.
    ;
    ; It lets you take an @[...] block, because a plain [...] block would be
    ; processed as a rule.  For the moment it quotes it for convenience.

    'fail combinator [
        return: "Divergent"
            []
        'reason [the-block!]
        <local> e
    ][
        e: make error! [
            type: 'User
            id: 'parse
            message: to text! reason
        ]
        set-location-of-error e binding of 'reason
        e.near: mold/limit input 80
        fail e
    ]
]


=== MAKE TUPLE! COMBINATOR SAME AS WORD! COMBINATOR ===

; There's no easy way to do this in a MAP!, like `word!: tuple!: ...` would
; work for OBJECT!.

default-combinators.(tuple!): :default-combinators.(word!)


=== COMPATIBILITY FOR NON-TAG KEYWORD FORMS ===

; !!! This has been deprecated.  But it's currently possible to get this
; just by saying `end: <end>` and `here: <here>`.

comment [
    default-combinators.('here): :default-combinators.<here>
    default-combinators.('end): :default-combinators.<end>
]


comment [combinatorize: func [

    {Analyze combinator parameters in rules to produce a specialized "parser"}

    return: "Parser function taking only input, returning value + remainder"
        [action!]
    advanced: [block!]

    combinator "Parser combinator taking input, but also other parameters"
        [action!]
    rules [block!]
    state "Parse State" [frame!]
    /value "Initiating value (if datatype)" [any-value!]
    /path "Invoking Path" [path!]
    <local> r f
][
    ; Combinators take arguments.  If the arguments are quoted, then they are
    ; taken literally from the rules feed.  If they are not quoted, they will
    ; be another "parser" generated from the rules.
    ;
    ; For instance: CHANGE takes two arguments.  The first is a parser and has
    ; to be constructed with PARSIFY from the rules.  But the replacement is a
    ; literal value, e.g.
    ;
    ;      >> data: "aaabbb"
    ;      >> parse data [change some "a" "literal" some "b"]
    ;      == "literalbbb"
    ;
    ; So we see that CHANGE got SOME "A" turned into a parser action, but it
    ; received "literal" literally.  The definition of the combinator is used
    ; to determine the arguments and which kind they are.

    if path [  ; was for /ONLY but not in use now, will be rethought
        fail "Refinements not supported currently with combinators"
    ]

    f: make frame! :combinator

    for-each param parameters of :combinator [
        case [
            param = 'input [
                ; All combinators should have an input.  But the
                ; idea is that we leave this unspecialized.
            ]
            param = '/remainder [
                ; The remainder is a return; responsibility of the caller, also
                ; left unspecialized.
            ]
            param = '/pending [
                ; same for pending, this is the data being gathered that may
                ; need to be discarded (gives impression of "rollback") and
                ; is the feature behind COLLECT etc.
            ]
            param = 'value [
                f.value: :value
            ]
            param = 'state [  ; the "state" is currently the UPARSE frame
                f.state: state
            ]
            quoted? param [  ; literal element captured from rules
                param: unquote param
                r: :rules.1
                any [
                    not ^r
                    find [, | ||] ^r
                ]
                then [
                    if not endable? in f param [
                        fail "Too few parameters for combinator"
                    ]
                    f.(param): null
                ]
                else [
                    ; We also allow skippable parameters, so that legacy
                    ; combinators can implement things like INTEGER! combinator
                    ; which takes another optional INTEGER! for end of range.
                    ;
                    ; !!! We use KIND OF :R here because TYPE OF can give a
                    ; quoted type.  This isn't supported by FIND atm.
                    ;
                    all [
                        skippable? in f param
                        not find (exemplar of action of f).(param) kind of :r
                    ] then [
                        f.(param): null
                    ]
                    else [
                        f.(param): :r
                        rules: next rules
                    ]
                ]
            ]
            refinement? param [
                ; Leave refinements alone, e.g. /only ... a general strategy
                ; would be needed for these if the refinements add parameters
                ; as to how they work.
            ]
            true [  ; another parser to combine with
                ;
                ; !!! At the moment we disallow SET with GROUP!.
                ; This could be more conservative to stop calling
                ; functions.  For now, just work around it.
                ;
                r: :rules.1
                any [
                    not ^r
                    find [, | ||] ^r
                ]
                then [
                    if not endable? in f param [
                        fail "Too few parameters for combinator"
                    ]
                    f.(param): null
                ]
                else [
                    f.(param): [# rules]: parsify state rules
                ]
            ]
        ]
    ]

    advanced: rules
    return make action! f
]]


parsify: func [
    {Transform one "step's worth" of rules into a parser combinator action}

    return: "Parser action for input processing corresponding to a full rule"
        [action!]
    advanced: "Rules position advanced past the elements used for the action"
        [block!]

    state "Parse state"
        [frame!]
    rules "Parse rules to (partially) convert to a combinator action"
        [block!]
    <local> r comb value
][
    r: rules.1

    ; The concept behind COMMA! is to provide a delimiting between rules.
    ; That is handled by the block combinator.  So if you see a thing like
    ; `[some, "a"]` in PARSIFY, that is just running out of turn.
    ;
    ; It may seem like making a dummy "comma combinator" for the comma
    ; type is a good idea.  But that's an unwanted axis of flexibility
    ; in this model...and also if we defer the error until the combinator
    ; is run, it might never be run.
    ;
    if comma? :r [
        fail "COMMA! can only be run between PARSE steps, not inside them"
    ]
    rules: my next

    case [
        word? :r [
            ;
            ; The first thing we do is see if the combinator list we are using
            ; has an entry for this word/symbol.
            ;
            if value: select state.combinators r [
                ;
                ; Combinators get "combinated" with the subsequent stream of
                ; rules to produce a parser instance.  This is the common case.
                ;
                if comb: match action! :value [
                    return [# advanced]: combinatorize :comb rules state
                ]

                ; Ordinary data values are dispatched to the combinator for
                ; that datatype.  This feature is useful for injecting simple
                ; definitions into the parser, like mapping ALPHA to the bitset
                ; for alphabetic characters.  Unlike binding, such definitions
                ; added to the combinator table are available globally while
                ; the parser runs.
                ;
                ; At the moment there are no "variadic" combinators, so these
                ; dispatches do not have access to the stream of subsequent
                ; rules...only the value itself.
                ;
                ; !!! Note: We have looked up a WORD! and found it literally in
                ; the combinator table.  But despite looking up a word, it
                ; does not get passed to the WORD! combinator in this case.
                ; The datatype handler is unconditionally called with no hook,
                ; as if the value had appeared literally in the rule stream.
                ;
                comb: select state.combinators type of :value
                return (
                    [# advanced]: combinatorize/value :comb rules state :value
                )
            ]

            ; Failing to find an entry in the combinator table, we fall back on
            ; checking to see if the word looks up to a variable via binding.
            ;
            value: get r else [
                fail [r "looked up to NULL in UPARSE"]
            ]

            ; Looking up to a combinator via variable is allowed, and will use
            ; COMBINATORIZE to permit access to consume parts of the subsequent
            ; rule stream.
            ;
            if comb: match action! :value [
                if combinator? :comb [
                    return [# advanced]: combinatorize :comb rules state
                ]

                let name: uppercase to text! r
                fail [
                    name "is not a COMBINATOR ACTION!"
                    "For non-combinator actions in UPARSE, use" :[name "/"]
                ]
            ]

            ; Any other values that we looked up as a variable will be
            ; sent to the WORD! combinator for processing.  This permits a
            ; word that looks up to a value to do something distinct from what
            ; the value would do literally in the stream.
            ;
            ; (For the most part, this should be used to limit what types are
            ; legal to fetch from words...but there may be cases where distinct
            ; non-erroring behavior is desired.)
            ;
            comb: select state.combinators word!
            return [# advanced]: combinatorize/value :comb rules state r
        ]

        path? :r [
            ;
            ; !!! Wild new feature idea: if a PATH! ends in a slash, assume it
            ; is an invocation of a normal function with the results of
            ; combinators as its arguments.
            ;
            ; This is variadic and hacked in, so it cannot be done via a
            ; combinator.  But we fall through to a PATH! combinator for paths
            ; that don't end in slashes.  This permits (among other things)
            ; the Rebol2 parse semantics to look up variables via PATH!.
            ;
            let f
            if blank? last r [
                if not action? let action: get :r [
                    fail "In UPARSE PATH ending in / must resolve to ACTION!"
                ]
                if not comb: select state.combinators action! [
                    fail "No ACTION! combinator, can't use PATH ending in /"
                ]

                ; !!! The ACTION! combinator has to be variadic, because the
                ; number of arguments it takes depends on the arguments of
                ; the action.  This requires design.  :-/
                ;
                ; For the moment, do something weird and customize the
                ; combinator with AUGMENT for each argument (parser1, parser2
                ; parser3).
                ;
                comb: adapt augment :comb collect [
                    let n: 1
                    for-each param parameters of :action [
                        if not path? param [
                            keep compose [
                                (to word! unspaced ["param" n]) [action!]
                            ]
                            n: n + 1
                        ]
                    ]
                ][
                    parsers: copy []

                    ; No RETURN visible in ADAPT.  :-/  Should we use ENCLOSE
                    ; to more legitimately get the frame as a parameter?
                    ;
                    let f: binding of 'param1

                    let n: 1
                    for-each param (parameters of :value) [
                        if not path? param [
                            append parsers ^f.(as word! unspaced ["param" n])
                            n: n + 1
                        ]
                    ]
                ]

                return [# advanced]:
                        combinatorize/value :comb rules state :action
            ]

            let word: ensure word! first r
            if comb: select state.combinators word [
                return [# advanced]: combinatorize/path :comb rules state r
            ]

            ; !!! Originally this would just say "unknown combinator" at this
            ; point, but for compatibility with historical Rebol we handle
            ; paths in UPARSE for now as being gotten as if they were tuples.
            ;
            r: get r else [fail [r "is NULL, not legal in UPARSE"]]
        ]

        ; !!! Here is where we would let GET-PATH! and GET-WORD! be used to
        ; subvert keywords if SEEK were universally adopted.
    ]

    ; Non-keywords are also handled as combinators, where we just pass the
    ; data value itself to the handler for that type.
    ;
    ; !!! This won't work with INTEGER!, as they are actually rules with
    ; arguments.  Does this mean the block rule has to hardcode handling of
    ; integers, or that when we do these rules they may have skippable types?

    if not comb: select state.combinators kind of :r [
        fail ["Unhandled type in PARSIFY:" kind of :r "-" mold :r]
    ]

    return [# advanced]: combinatorize/value :comb rules state r
]


=== ENTRY POINTS: PARSE*, PARSE, MATCH-PARSE ===

; The historical Redbol PARSE was focused on returning a LOGIC! so that you
; could write `if parse data rules [...]` and easily react to finding out if
; the rules matched the input to completion.
;
; While this bias stuck with UPARSE in the beginning, the deeper power of
; returning the evaluated result made it hugely desirable as the main form
; that owns the word "PARSE".  A version that does not check to completion
; is fundamentally the most powerful, since it can just make HERE the last
; result...and then check that the result is at the tail.  Other variables
; can be set as well.
;
; So this formulates everything on top of a PARSE* that returns the sythesized
; result

parse*: func [
    {Process as much of the input as parse rules consume (see also PARSE)}

    return: "Synthesized value from last match rule, or NULL if rules failed"
        [<opt> any-value!]
    furthest: "Furthest input point reached by the parse"
        [any-series!]
    pending: "Request unprocessed pending items (default errors if any)"
        [blank! block!]

    input "Input data"
        [<blank> any-series! url! any-sequence!]
    rules "Block of parse rules"
        [block!]

    /combinators "List of keyword and datatype handlers used for this parse"
        [map!]
    /case "Do case-sensitive matching"
    /fully "Return NULL if the end of series is not reached"
    /part "FAKE /PART FEATURE - runs on a copy of the series!"
        [integer! any-series!]

    /verbose "Print some additional debug information"

    <local> loops
][
    ; PATH!s, TUPLE!s, and URL!s are read only and don't have indices.  But we
    ; want to be able to parse them, so make them read-only series aliases:
    ;
    ; https://forum.rebol.info/t/1276/16
    ;
    lib.case [  ; !!! Careful... CASE is a refinement!
        any-sequence? input [input: as block! input]
        url? input [input: as text! input]
    ]

    loops: copy []  ; need new loop copy each invocation

    ; We put an implicit PHASE bracketing the whole of UPARSE* so that the
    ; <delay> groups will be executed.
    ;
    rules: :['phase rules]

    ; !!! Red has a /PART feature and so in order to run the tests pertaining
    ; to that we go ahead and fake it.  Actually implementing /PART would be
    ; quite a tax on the combinators...so thinking about a system-wide slice
    ; capability would be more sensible.  The series will not have the same
    ; identity, so mutating operations will mutate the wrong series...we
    ; could copy back, but that just shows what a slippery slope this is.
    ;
    if part [
        input: copy/part input part
    ]

    combinators: default [default-combinators]

    ; The COMBINATOR definition makes a function which is hooked with code
    ; that will mark the furthest point reached by any match.
    ;
    if wanted? 'furthest [
        furthest: input
    ] else [
        furthest: null  ; all [state.furthest...] first check for if updating
    ]

    ; Each UPARSE operation can have a different set of combinators in
    ; effect.  So it's necessary to need to have some way to get at that
    ; information.  For now, we use the FRAME! of the parse itself as the
    ; way that data is threaded...as it gives access to not just the
    ; combinators, but also the /VERBOSE or other settings...we can add more.
    ;
    let state: binding of 'return

    let f: make frame! :combinators.(block!)
    f.state: state
    f.input: input
    f.value: rules
    f.remainder: let pos

    ; !!! When we get to the end here, success is kind of the only moment when
    ; we can know it's okay to run the "only if this completely succeeds" stuff
    ; if there's not some other checkpoint.
    ;
    f.pending: let subpending

    let synthesized': (^ eval f)
    assert [empty? state.loops]

    if null? synthesized' [
        return null  ; match failure (as opposed to success, w/null result)
    ]

    if wanted? 'pending [
        pending: subpending
    ] else [
        all [subpending, not empty? subpending] then [
            fail "Residual items accumulated in pending array"
        ]
    ]

    all [fully, not tail? pos] then [
        return null  ; full parse was requested but tail was not reached
    ]

    if synthesized' = @void [
        ;
        ; We can't return "invisible intent" and still convey that the parse
        ; succeeded.  So a UPARSE that succeeds is a bit like a branch that
        ; runs.  You lose the void, and it collapses to none.  So invisibility
        ; has to stay inside the UPARSE ruleverse.
        ;
        return ~  ; Need something nothing-like but that won't trigger an ELSE
    ]

    return isotopify-if-falsey unmeta synthesized'
]

parse: (comment [redescribe [  ; redescribe not working at the moment (?)
    {Process input in the parse dialect, must match to end (see also UPARSE*)}
] ]
    :parse*/fully
)

sys.util.parse: :parse  ; !!! expose UPARSE to SYS.UTIL module, hack...

match-parse: (comment [redescribe [  ; redescribe not working at the moment (?)
    {Process input in the parse dialect, input if match (see also UPARSE*)}
] ]
    ; Note: Users could write `parse data [...rules... || <input>]` and get
    ; the same effect generally.
    ;
    ; !!! It might be tempting to write this as an ADAPT which changes the
    ; rules to be:
    ;
    ;    rules: reduce [rules <input>]
    ;
    ; But if someone changed the meaning of <input> with different /COMBINATORS
    ; that would not work.  This method will work regardless.
    ;
    enclose :parse*/fully func [f [frame!]] [
        let input: f.input  ; DO FRAME! invalidates args; cache for returning

        return all [^ eval f, input]
    ]
)


=== "USING" FEATURE ===

; !!! This operation will likely take over the name USE.  It is put here since
; the UPARSE tests involve it.
;
using: func [
    return: <none>  ; should it return a value?  (e.g. the object?)
    obj [<blank> object!]
][
    add-use-object (binding of 'obj) obj
]