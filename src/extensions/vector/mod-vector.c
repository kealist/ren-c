//
//  File: %mod-vector.c
//  Summary: "VECTOR! extension main C file"
//  Section: Extension
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 Atronix Engineering
// Copyright 2012-2019 Rebol Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//=////////////////////////////////////////////////////////////////////////=//
//
// See notes in %src/extensions/vector/README.md

#include "sys-core.h"

#include "tmp-mod-vector.h"

#include "sys-vector.h"


//
//  register-vector-hooks: native [
//
//  {Make the VECTOR! datatype work with GENERIC actions, comparison ops, etc}
//
//      return: [void!]
//  ]
//
REBNATIVE(register_vector_hooks)
{
    VECTOR_INCLUDE_PARAMS_OF_REGISTER_VECTOR_HOOKS;

    // !!! See notes on Hook_Datatype for this poor-man's substitute for a
    // coherent design of an extensible object system (as per Lisp's CLOS)
    //
    Hook_Datatype(
        REB_VECTOR,
        &T_Vector,
        &PD_Vector,
        &CT_Vector,
        &MAKE_Vector,
        &TO_Vector,
        &MF_Vector
    );

    return Init_Void(D_OUT);
}


//
//  unregister-vector-hooks: native [
//
//  {Remove behaviors for VECTOR! added by REGISTER-VECTOR-HOOKS}
//
//      return: [void!]
//  ]
//
REBNATIVE(unregister_vector_hooks)
{
    VECTOR_INCLUDE_PARAMS_OF_UNREGISTER_VECTOR_HOOKS;

    Unhook_Datatype(REB_VECTOR);

    return Init_Void(D_OUT);
}
