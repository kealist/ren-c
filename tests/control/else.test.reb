(
    success: <bad>
    if 1 > 2 [success: false] else [success: true]
    success
)
(
    success: <bad>
    if 1 < 2 [success: true] else [success: false]
    success
)
(
    success: <bad>
    if not 1 > 2 [success: true] else [success: false]
    success
)
(
    success: <bad>
    if not 1 < 2 [success: false] else [success: true]
    success
)
(
    success: <bad>
    if true (does [success: true])
    success
)
(
    success: true
    if false (does [success: false])
    success
)

[https://github.com/metaeducation/ren-c/issues/510 (
    c: func [return: [integer!] i] [
        return if i < 15 [30] else [4]
    ]

    d: func [return: [integer!] i] [
        return (if i < 15 [30] else [4])
    ]

    did all [
        30 = c 10
        4 = c 20
        30 = d 10
        4 = d 20
    ]
)]

; Hard quotes need to account for enfix deferral
(
    foo: func [return: [integer!] y] [return the 1 then (x -> [x + y])]
    bar: func [return: [integer!] y] [return 1 then (x -> [x + y])]
    did all [
        11 = foo 10
        1 = bar 10
    ]
)

; The above should work whether you use a GROUP! or not (-> quote left wins)
(
    foo: func [return: [integer!] y] [return the 1 then x -> [x + y]]
    bar: func [return: [integer!] y] [return 1 then x -> [x + y]]
    did all [
        11 = foo 10
        1 = bar 10
    ]
)

(
    <isotope> = (~null~ then [<isotope>])
)

; Although branches can be triggered by ~null~ isotopes, if functions ask to
; receive the value it is decayed, so they do not have to be ^META.  But if
; they *are* meta then the true state is passed through.
[
    (~null~ then x -> [x = null])
    (~null~ then ^x -> [x = the ~null~])
    ('~null~ then x -> [x = the ~null~])
    ('~null~ then ^x -> [x = the '~null~])

    (catch [~null~ also x -> [throw (x = null)]])
    (catch [~null~ also ^x -> [throw (x = the ~null~)]])
    (catch ['~null~ also ^x -> [throw (x = the '~null~)]])

    (~null~ *else x -> [null = x])
    (null *else ^x -> [null = x])
    (~null~ *else ^x -> ['~null~ = x])
]

; Variant forms react to ~null~ isotopes as if they were null.  This can be
; useful in chaining scenarios.
;
; https://forum.rebol.info/t/why-then-and-else-are-mutually-exclusive/1080/9
[
    (~null~ *then [fail "shouldn't run"] else [true])
    (~null~ *also [fail "shouldn't run"] *else [true])
    (~null~ *else [true])
]
