[
    success: <bad>
    if 1 > 2 [success: false] else [success: true]
    success
][
    success: <bad>
    if 1 < 2 [success: true] else [success: false]
    success
][
    success: <bad>
    unless 1 > 2 [success: true] else [success: false]
    success
][
    success: <bad>
    unless 1 < 2 [success: false] else [success: true]
    success
][
    success: <bad>
    if true does [success: true]
    success
][
    success: true
    if false does [success: false]
    success
]

[
    ; Don't want `return if ... else ...` to act as `(return if ...) else ...`
    ; https://github.com/metaeducation/ren-c/issues/510

    c: func [i] [
        return if i < 15 [30] else [4]
    ]

    a: all? [
        30 = c 10
        4 = c 20
    ]
]
