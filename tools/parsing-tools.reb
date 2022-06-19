REBOL [
    Title: "Parsing tools"
    Type: module
    Name: Parsing-Tools
    Rights: {
        Rebol is Copyright 1997-2015 REBOL Technologies
        REBOL is a trademark of REBOL Technologies

        Ren-C is Copyright 2015-2018 MetaEducation
    }
    License: {
        Licensed under the Apache License, Version 2.0
        See: http://www.apache.org/licenses/LICENSE-2.0
    }
    Author: "@codebybrett"
    Version: 2.100.0
    Needs: 2.100.100
    Purpose: {
        These are some common routines used to assist parsing tasks.
    }
]

import <bootstrap-shim.r>

seek: []  ; Temporary measure, SEEK as no-op in bootstrap

export parsing-at: func [
    {Make rule that evaluates a block for next input position, fails otherwise}
    return: [block!]
    'word [word!] {Word set to input position (will be local).}
    block [block!]
        {Block to evaluate. Return next input position, or blank/false.}
    /end {Drop the default tail check (allows evaluation at the tail).}
][
    return use [result position][
        block: compose2/only [try (as group! block)]
        if not end [
            block: compose2/deep [try if not tail? (word) [((block))]]
        ]
        block: compose2/deep [
            result: either position: ((block)) [[
                :position  ; seek
            ]] [[
                end skip
            ]]
        ]
        use compose2 [(word)] compose2/deep [
            [
                (as set-word! :word)  ; <here>
                (as group! block) result
            ]
        ]
    ]
]
