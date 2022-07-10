; %parse-further.test.reb
;
; Historical Rebol's ANY and SOME operations had a built in notion of checking
; for advancement.  This introduced a subtle and easy to trip up on notion
; of whether a rule advanced, so rules that didn't want to stop the iteration
; but removed input etc. would create an issue.  FURTHER takes the test
; for advancement out and makes it usable with any rule.

(null = parse "" [further [opt "a" opt "b"] ("at least one")])
("at least 1" = parse "a" [further [opt "a" opt "b"] ("at least 1")])
("at least 1" = parse "a" [further [opt "a" opt "b"] ("at least 1")])
("at least 1" = parse "ab" [further [opt "a" opt "b"] ("at least 1")])

(none? parse "" [repeat (#) some further [to <end>]])

[https://github.com/red/red/issues/3927
    (didn't parse "bx" [some further [not "b" | <any>]])
]

; Only SOME is needed in Red, but OPT SOME FURTHER is needed here.  But the
; reasoning is good for why UPARSE operates how it does.
;
; "It's a really convoluted way of thinking of what you're doing here as
;  'some number of matches... including a non-match that doesn't advance of
;  #{0B} counting as at least one match, so the SOME doesn't fail to match.
;  but if it doesn't advance, even though the one time succeeded count it
;  as a break of the iteration and yield success'. :-(  Who wants to think
;  like that?  Why use alternates instead of just saying it's zero-or-more
;  #"^L", then followed by a single not #{0B}?"
[
    ('a == parse [a a] [opt some further ['c | not 'b] 2 <any>])
    (#a == parse "aa" [opt some further [#c | not #b] 2 <any>])
    (10 == parse #{0A0A} [opt some further [#"^L" | not #{0B}] 2 <any>])

    ; Saner way to write it... no need for FURTHER.
    ;
    ('a == parse [a a] [opt some 'c, not 'b, 2 <any>])
    (#a == parse "aa" [opt some #c, not #b, 2 <any>])
    (10 == parse #{0A0A} [opt some #"^L", not #{0B}, 2 <any>])
]
