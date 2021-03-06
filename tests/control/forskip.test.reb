; functions/control/for-skip.r
(
    blk: copy out: copy []
    for i 1 25 1 [append blk i]
    for-skip blk 3 [append out blk/1]
    out = [1 4 7 10 13 16 19 22 25]
)
; cycle return value
(
    blk: [1 2 3 4]
    true = for-skip blk 1 [true]
)
(
    blk: [1 2 3 4]
    false = for-skip blk 1 [false]
)
; break cycle
(
    str: "abcdef"
    for-skip str 2 [
        if #"c" = char: str/1 [break]
    ]
    char = #"c"
)
; break return value
(
    blk: [1 2 3 4]
    null? for-skip blk 2 [break]
)
; continue cycle
(
    success: true
    x: "a"
    for-skip x 1 [continue success: false]
    success
)
; zero repetition
(
    success: true
    blk: []
    for-skip blk 1 [success: false]
    success
)
; Test that return stops the loop
(
    blk: [1]
    f1: func [] [for-skip blk 2 [return 1 2]]
    1 = f1
)
; Test that errors do not stop the loop and errors can be returned
(
    num: 0
    blk: [1 2]
    e: for-skip blk 1 [num: first blk trap [1 / 0]]
    all [error? e num = 2]
)
; recursivity
(
    num: 0
    blk1: [1 2 3 4 5]
    blk2: [6 7]
    for-skip blk1 1 [
        num: num + first blk1
        for-skip blk2 1 [num: num + first blk2]
    ]
    num = 80
)
