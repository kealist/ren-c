; functions/control/apply.r
[#44 (
    error? trap [r3-alpha-apply 'append/only [copy [a b] 'c]]
)]
(1 == r3-alpha-apply :subtract [2 1])
(1 = (r3-alpha-apply :- [2 1]))
(error? trap [r3-alpha-apply func [a] [a] []])
(error? trap [r3-alpha-apply/only func [a] [a] []])

; CC#2237
(error? trap [r3-alpha-apply func [a] [a] [1 2]])
(error? trap [r3-alpha-apply/only func [a] [a] [1 2]])

(error? r3-alpha-apply :make [error! ""])

(true = r3-alpha-apply func [/a] [a] [true])
(false == r3-alpha-apply func [/a] [a] [false])
(false == r3-alpha-apply func [/a] [a] [])
(true = r3-alpha-apply/only func [/a] [a] [true])
; the word 'false
(true = r3-alpha-apply/only func [/a] [a] [false])
(false == r3-alpha-apply/only func [/a] [a] [])
(use [a] [a: true true = r3-alpha-apply func [/a] [a] [a]])
(use [a] [a: false false == r3-alpha-apply func [/a] [a] [a]])
(use [a] [a: false true = r3-alpha-apply func [/a] [a] ['a]])
(use [a] [a: false true = r3-alpha-apply func [/a] [a] [/a]])
(use [a] [a: false true = r3-alpha-apply/only func [/a] [a] [a]])
(group! == r3-alpha-apply/only (specialize 'of [property: 'type]) [()])
([1] == head of r3-alpha-apply :insert [copy [] [1] blank blank blank])
([1] == head of r3-alpha-apply :insert [copy [] [1] blank blank false])
([[1]] == head of r3-alpha-apply :insert [copy [] [1] blank blank true])
(action! == r3-alpha-apply (specialize 'of [property: 'type]) [:print])
(get-word! == r3-alpha-apply/only (specialize 'of [property: 'type]) [:print])

;-- #1760 --

(
    1 == eval func [] [r3-alpha-apply does [] [return 1] 2]
)
(
    1 == eval func [] [r3-alpha-apply func [a] [a] [return 1] 2]
)
(
    1 == eval func [] [r3-alpha-apply does [] [return 1]]
)
(
    1 == eval func [] [r3-alpha-apply func [a] [a] [return 1]]
)
(
    1 == eval func [] [r3-alpha-apply func [a b] [a] [return 1 2]]
)
(
    1 == eval func [] [r3-alpha-apply func [a b] [a] [2 return 1]]
)

; EVAL/ONLY
(
    o: make object! [a: 0]
    b: eval/only (quote o/a:) 1 + 2
    all [o/a = 1 | b = 1] ;-- above acts as `b: (eval/only (quote o/a:) 1) + 2`
)
(
    a: func [b c :d] [reduce [b c d]]
    [1 + 2] = (eval/only :a 1 + 2)
)

(
    null? r3-alpha-apply func [
        return: [<opt> any-value!]
        x [<opt> any-value!]
    ][
        get 'x
    ][
        ()
    ]
)
(
    null? r3-alpha-apply func [
        return: [<opt> any-value!]
        'x [<opt> any-value!]
    ][
        get 'x
    ][
        ()
    ]
)
(
    null? r3-alpha-apply func [
        return: [<opt> any-value!]
        x [<opt> any-value!]
    ][
        return get 'x
    ][
        ()
    ]
)
(
    null? r3-alpha-apply func [
        return: [<opt> any-value!]
        'x [<opt> any-value!]
    ][
        return get 'x
    ][
        ()
    ]
)
(
    error? r3-alpha-apply func ['x [<opt> any-value!]] [
        return get 'x
    ][
        make error! ""
    ]
)
(
    error? r3-alpha-apply/only func [x [<opt> any-value!]] [
        return get 'x
    ] head of insert copy [] make error! ""
)
(
    error? r3-alpha-apply/only func ['x [<opt> any-value!]] [
        return get 'x
    ] head of insert copy [] make error! ""
)
(use [x] [x: 1 strict-equal? 1 r3-alpha-apply func ['x] [:x] [:x]])
(use [x] [x: 1 strict-equal? 1 r3-alpha-apply func ['x] [:x] [:x]])
(
    use [x] [
        x: 1
        strict-equal? first [:x] r3-alpha-apply/only func [:x] [:x] [:x]
    ]
)
(
    use [x] [
        unset 'x
        strict-equal? first [:x] r3-alpha-apply/only func ['x [<opt> any-value!]] [
            return get 'x
        ] [:x]
    ]
)
(use [x] [x: 1 strict-equal? 1 r3-alpha-apply func [:x] [:x] [x]])
(use [x] [x: 1 strict-equal? 'x r3-alpha-apply func [:x] [:x] ['x]])
(use [x] [x: 1 strict-equal? 'x r3-alpha-apply/only func [:x] [:x] [x]])
(use [x] [x: 1 strict-equal? 'x r3-alpha-apply/only func [:x] [return :x] [x]])
(
    use [x] [
        unset 'x
        strict-equal? 'x r3-alpha-apply/only func ['x [<opt> any-value!]] [
            return get 'x
        ] [x]
    ]
)

; MAKE FRAME! :RETURN should preserve binding in the FUNCTION OF the frame
;
(1 == eval func [] [r3-alpha-apply :return [1] 2])

(false == r3-alpha-apply/only func [/a] [a] [#[false]])
(group! == r3-alpha-apply/only :type-of [()])
