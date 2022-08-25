;
; %match.test.reb
;
; MATCH started out as a userspace function, but gained frequent enough use
; to become a native.  It is theorized that MATCH will evolve into the
; tool for checking arguments in function specs.
;

(10 = match integer! 10)
(null = match integer! "ten")

("ten" = match [integer! text!] "ten")
(20 = match [integer! text!] 20)
(null = match [integer! text!] <tag>)

(10 = match :even? 10)
(null = match :even? 3)


('~blank~ = ^ match blank! _)
(null = match blank! 10)
(null = match blank! false)


; Falsey things are turned to BAD-WORD! isotopes in order to avoid cases like:
;
;     if match logic! flag [...]
;
; But can still be tested for with DID and DIDN'T since they are isotope
; tolerant and NULL-reactive, and also can be used with THEN and ELSE.
[
    ('~null~ = ^ match null null)
    ('~blank~ = ^ match blank! blank)
    (true = match logic! true)
    ('~false~ = ^ match logic! false)
]

[
    (10 = match integer! 10)
    (null = match integer! <tag>)

    ('a/b: = match any-path! 'a/b:)
    ('a/b: = match any-sequence! 'a/b:)
    (null = match any-array! 'a/b:)
]

; ENSURE is a version of MATCH that fails vs. returning NULL on no match
[
    (error? trap [ensure action! 10])
    (10 = ensure integer! 10)
]

; NON is an inverted form of ENSURE, that FAILs when the argument *matches*
[
    (null = non action! :append)
    (10 = non action! 10)

    (null = non integer! 10)
    (:append = non integer! :append)

    (10 = non null 10)

    (null = non null null)
    (null = non logic! false)
]

; PROHIBIT is an inverted version of ENSURE, where it must not match
; probably needs a better name, even ENSURE-NOT is likely clearer
[
    (error? trap [prohibit action! :append])
    (10 = prohibit action! 10)

    (error? trap [prohibit integer! 10])
    (:append = prohibit integer! :append)

    (10 = prohibit null 10)

    (error? trap [prohibit null null])
    (error? trap [prohibit logic! false])
]


; MUST is an optimized form of NON NULL
[
    ("bc" = must find "abc" "b")
    (error? trap [must find "abc" "q"])
]


; MATCH was an early function for trying a REFRAMER-like capacity for
; building a frame of an invocation, stealing its first argument, and then
; returning that in the case of a match.  But now that REFRAMER exists,
; the idea of having that feature implemented in core functions has fallen
; from favor.
;
; Here we see the demo done with a reframer to make MATCH+ as a proof of
; concept of how it would be done if you wanted it.
[
    (match+: reframer func [f [frame!] <local> p] [
        p: f.(first parameters of action of f)  ; get the first parameter
        if did do f [
            return p
        ] else [
            return null
        ] ; evaluate to parameter if operation succeeds
    ]
    true)

    (null = match+ parse3 "aaa" [some "b"])
    ("aaa" = match+ parse3 "aaa" [some "a"])
]
