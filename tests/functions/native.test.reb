; datatypes/native.r
(action? :reduce)
(not action? 1)
(isotope! = kind of :reduce)
[#1659 (
    ; natives are active
    same? blank! do reduce [
        (unrun specialize :of [property: 'type]) blank
    ]
)]
