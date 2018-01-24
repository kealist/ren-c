; logic!
[true and+ true = true]
[true and+ false = false]
[false and+ true = false]
[false and+ false = false]

; integer!
[1 and+ 1 = 1]
[1 and+ 0 = 0]
[0 and+ 1 = 0]
[0 and+ 0 = 0]
[1 and+ 2 = 0]
[2 and+ 1 = 0]
[2 and+ 2 = 2]

; char!
[#"^(00)" and+ #"^(00)" = #"^(00)"]
[#"^(01)" and+ #"^(00)" = #"^(00)"]
[#"^(00)" and+ #"^(01)" = #"^(00)"]
[#"^(01)" and+ #"^(01)" = #"^(01)"]
[#"^(01)" and+ #"^(02)" = #"^(00)"]
[#"^(02)" and+ #"^(02)" = #"^(02)"]

; tuple!
[0.0.0 and+ 0.0.0 = 0.0.0]
[1.0.0 and+ 1.0.0 = 1.0.0]
[2.0.0 and+ 2.0.0 = 2.0.0]
[255.255.255 and+ 255.255.255 = 255.255.255]

; binary!
[#{030000} and+ #{020000} = #{020000}]

; !!! arccosing tests that somehow are in and.test.reb
[0 = arccosine 1]
[0 = arccosine/radians 1]
[30 = arccosine (square-root 3) / 2]
[(pi / 6) = arccosine/radians (square-root 3) / 2]
[45 = arccosine (square-root 2) / 2]
[(pi / 4) = arccosine/radians (square-root 2) / 2]
[60 = arccosine 0.5]
[(pi / 3) = arccosine/radians 0.5]
[90 = arccosine 0]
[(pi / 2) = arccosine/radians 0]
[180 = arccosine -1]
[pi = arccosine/radians -1]
[150 = arccosine (square-root 3) / -2]
[((pi * 5) / 6) = arccosine/radians (square-root 3) / -2]
[135 = arccosine (square-root 2) / -2]
[((pi * 3) / 4) = arccosine/radians (square-root 2) / -2]
[120 = arccosine -0.5]
[((pi * 2) / 3) = arccosine/radians -0.5]
[error? try [arccosine 1.1]]
[error? try [arccosine -1.1]]
