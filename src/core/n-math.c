//
//  File: %n-math.c
//  Summary: "native functions for math"
//  Section: natives
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2016 Rebol Open Source Contributors
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
// See also: the numeric datatypes
//

#include "sys-core.h"
#include "tmp-comptypes.h"
#include "sys-deci-funcs.h"

#include <math.h>
#include <float.h>

#define LOG2    0.6931471805599453
#define EPS     2.718281828459045235360287471

extern const double pi1;
const double pi1 = 3.14159265358979323846;
const double pi2 = 2.0 * 3.14159265358979323846;

#ifndef DBL_EPSILON
#define DBL_EPSILON 2.2204460492503131E-16
#endif

#define AS_DECIMAL(n) (IS_INTEGER(n) ? (REBDEC)VAL_INT64(n) : VAL_DECIMAL(n))

enum {SINE, COSINE, TANGENT};


//
//  Trig_Value: C
// 
// Convert integer arg, if present, to decimal and convert to radians
// if necessary.  Clip ranges for correct REBOL behavior.
//
static REBDEC Trig_Value(const REBVAL *value, REBOOL degrees, REBCNT which)
{
    REBDEC dval = AS_DECIMAL(value);

    if (degrees) {
        /* get dval between -360.0 and 360.0 */
        dval = fmod (dval, 360.0);

        /* get dval between -180.0 and 180.0 */
        if (fabs (dval) > 180.0) dval += dval < 0.0 ? 360.0 : -360.0;
        if (which == TANGENT) {
            /* get dval between -90.0 and 90.0 */
            if (fabs (dval) > 90.0) dval += dval < 0.0 ? 180.0 : -180.0;
        } else if (which == SINE) {
            /* get dval between -90.0 and 90.0 */
            if (fabs (dval) > 90.0) dval = (dval < 0.0 ? -180.0 : 180.0) - dval;
        }
        dval = dval * pi1 / 180.0; // to radians
    }

    return dval;
}


//
//  Arc_Trans: C
//
static void Arc_Trans(REBVAL *out, const REBVAL *value, REBOOL degrees, REBCNT kind)
{
    REBDEC dval = AS_DECIMAL(value);
    if (kind != TANGENT && (dval < -1 || dval > 1)) fail (Error(RE_OVERFLOW));

    if (kind == SINE) dval = asin(dval);
    else if (kind == COSINE) dval = acos(dval);
    else dval = atan(dval);

    if (degrees) dval = dval * 180.0 / pi1; // to degrees

    SET_DECIMAL(out, dval);
}


//
//  cosine: native [
//  
//  "Returns the trigonometric cosine."
//  
//      value [any-number!] "In degrees by default"
//      /radians "Value is specified in radians"
//  ]
//
REBNATIVE(cosine)
{
    PARAM(1, value);
    REFINE(2, radians);

    REBDEC dval = cos(Trig_Value(ARG(value), NOT(REF(radians)), COSINE));
    if (fabs(dval) < DBL_EPSILON) dval = 0.0;
    SET_DECIMAL(D_OUT, dval);
    return R_OUT;
}


//
//  sine: native [
//  
//  "Returns the trigonometric sine."
//  
//      value [any-number!] "In degrees by default"
//      /radians "Value is specified in radians"
//  ]
//
REBNATIVE(sine)
{
    PARAM(1, value);
    REFINE(2, radians);

    REBDEC dval = sin(Trig_Value(ARG(value), NOT(REF(radians)), SINE));
    if (fabs(dval) < DBL_EPSILON) dval = 0.0;
    SET_DECIMAL(D_OUT, dval);
    return R_OUT;
}


//
//  tangent: native [
//  
//  "Returns the trigonometric tangent."
//  
//      value [any-number!] "In degrees by default"
//      /radians "Value is specified in radians"
//  ]
//
REBNATIVE(tangent)
{
    PARAM(1, value);
    REFINE(2, radians);

    REBDEC dval = Trig_Value(ARG(value), NOT(REF(radians)), TANGENT);
    if (Eq_Decimal(fabs(dval), pi1 / 2.0)) fail (Error(RE_OVERFLOW));
    SET_DECIMAL(D_OUT, tan(dval));
    return R_OUT;
}


//
//  arccosine: native [
//  
//  {Returns the trigonometric arccosine (in degrees by default).}
//  
//      value [any-number!]
//      /radians "Returns result in radians"
//  ]
//
REBNATIVE(arccosine)
{
    PARAM(1, value);
    REFINE(2, radians);

    Arc_Trans(D_OUT, ARG(value), NOT(REF(radians)), COSINE);
    return R_OUT;
}


//
//  arcsine: native [
//  
//  {Returns the trigonometric arcsine (in degrees by default).}
//  
//      value [any-number!]
//      /radians "Returns result in radians"
//  ]
//
REBNATIVE(arcsine)
{
    PARAM(1, value);
    REFINE(2, radians);

    Arc_Trans(D_OUT, ARG(value), NOT(REF(radians)), SINE);
    return R_OUT;
}


//
//  arctangent: native [
//  
//  {Returns the trigonometric arctangent (in degrees by default).}
//  
//      value [any-number!]
//      /radians "Returns result in radians"
//  ]
//
REBNATIVE(arctangent)
{
    PARAM(1, value);
    REFINE(2, radians);

    Arc_Trans(D_OUT, ARG(value), NOT(REF(radians)), TANGENT);
    return R_OUT;
}


//
//  exp: native [
//  
//  {Raises E (the base of natural logarithm) to the power specified}
//  
//      power [any-number!]
//  ]
//
REBNATIVE(exp)
{
    REBDEC  dval = AS_DECIMAL(D_ARG(1));
    static REBDEC eps = EPS;

    dval = pow(eps, dval);
//!!!!  Check_Overflow(dval);
    SET_DECIMAL(D_OUT, dval);
    return R_OUT;
}


//
//  log-10: native [
//  
//  "Returns the base-10 logarithm."
//  
//      value [any-number!]
//  ]
//
REBNATIVE(log_10)
{
    REBDEC dval = AS_DECIMAL(D_ARG(1));
    if (dval <= 0) fail (Error(RE_POSITIVE));
    SET_DECIMAL(D_OUT, log10(dval));
    return R_OUT;
}


//
//  log-2: native [
//  
//  "Return the base-2 logarithm."
//  
//      value [any-number!]
//  ]
//
REBNATIVE(log_2)
{
    REBDEC dval = AS_DECIMAL(D_ARG(1));
    if (dval <= 0) fail (Error(RE_POSITIVE));
    SET_DECIMAL(D_OUT, log(dval) / LOG2);
    return R_OUT;
}


//
//  log-e: native [
//  
//  {Returns the natural (base-E) logarithm of the given value}
//  
//      value [any-number!]
//  ]
//
REBNATIVE(log_e)
{
    REBDEC dval = AS_DECIMAL(D_ARG(1));
    if (dval <= 0) fail (Error(RE_POSITIVE));
    SET_DECIMAL(D_OUT, log(dval));
    return R_OUT;
}


//
//  square-root: native [
//  
//  "Returns the square root of a number."
//  
//      value [any-number!]
//  ]
//
REBNATIVE(square_root)
{
    REBDEC dval = AS_DECIMAL(D_ARG(1));
    if (dval < 0) fail (Error(RE_POSITIVE));
    SET_DECIMAL(D_OUT, sqrt(dval));
    return R_OUT;
}


//
//  shift: native [
//  
//  {Shifts an integer left or right by a number of bits.}
//  
//      value [integer!]
//      bits [integer!] "Positive for left shift, negative for right shift"
//      /logical "Logical shift (sign bit ignored)"
//  ]
//
REBNATIVE(shift)
{
    PARAM(1, value);
    PARAM(2, bits);
    REFINE(3, logical);

    REBI64 b = VAL_INT64(ARG(bits));
    REBVAL *a = ARG(value);
    REBU64 c, d;

    if (b < 0) {
        // this is defined:
        c = - cast(REBU64, b);
        if (c >= 64) {
            if (REF(logical)) VAL_INT64(a) = 0;
            else VAL_INT64(a) >>= 63;
        } else {
            if (REF(logical))
                VAL_INT64(a) = cast(REBU64, VAL_INT64(a)) >> c;
            else
                VAL_INT64(a) >>= cast(REBI64, c);
        }
    } else {
        if (b >= 64) {
            if (REF(logical)) VAL_INT64(a) = 0;
            else if (VAL_INT64(a)) fail (Error(RE_OVERFLOW));
        } else
            if (REF(logical))
                VAL_INT64(a) = cast(REBU64, VAL_INT64(a)) << b;
            else {
                c = cast(REBU64, MIN_I64) >> b;
                d = VAL_INT64(a) < 0
                    ? - cast(REBU64, VAL_INT64(a))
                    : cast(REBU64, VAL_INT64(a));
                if (c <= d) {
                    if ((c < d) || (VAL_INT64(a) >= 0))
                        fail (Error(RE_OVERFLOW));

                    VAL_INT64(a) = MIN_I64;
                }
                else
                    VAL_INT64(a) <<= b;
            }
    }

    *D_OUT = *ARG(value);
    return R_OUT;
}


//
//  Compare_Modify_Values: C
// 
// Compare 2 values depending on level of strictness.  It leans
// upon the per-type comparison functions (that have a more typical
// interface of returning [1, 0, -1] and taking a CASE parameter)
// but adds a layer of being able to check for specific types
// of equality...which those comparison functions do not discern.
// 
// Strictness:
//     0 - coersed equality
//     1 - equivalence
//     2 - strict equality
//     3 - same (identical bits)
// 
//    -1 - greater or equal
//    -2 - greater
// 
// !!! This routine (may) modify the value cells for 'a' and 'b' in
// order to coerce them for easier comparison.  Most usages are
// in native code that can overwrite its argument values without
// that being a problem, so it doesn't matter.
//
REBINT Compare_Modify_Values(REBVAL *a, REBVAL *b, REBINT strictness)
{
    REBCNT ta = VAL_TYPE(a);
    REBCNT tb = VAL_TYPE(b);
    REBCTF code;
    REBINT result;

    if (ta != tb) {
        if (strictness > 1) return 0;

        switch (ta) {
        case REB_INTEGER:
            if (tb == REB_DECIMAL || tb == REB_PERCENT) {
                REBDEC dec_a = cast(REBDEC, VAL_INT64(a));
                SET_DECIMAL(a, dec_a);
                goto compare;
            }
            else if (tb == REB_MONEY) {
                deci amount = int_to_deci(VAL_INT64(a));
                SET_MONEY_AMOUNT(a, amount);
                goto compare;
            }
            break;

        case REB_DECIMAL:
        case REB_PERCENT:
            if (tb == REB_INTEGER) {
                REBDEC dec_b = cast(REBDEC, VAL_INT64(b));
                SET_DECIMAL(b, dec_b);
                goto compare;
            }
            else if (tb == REB_MONEY) {
                deci amount = decimal_to_deci(VAL_DECIMAL(a));
                SET_MONEY_AMOUNT(a, amount);
                goto compare;
            }
            else if (tb == REB_DECIMAL || tb == REB_PERCENT) // equivalent types
                goto compare;
            break;

        case REB_MONEY:
            if (tb == REB_INTEGER) {
                deci amount = int_to_deci(VAL_INT64(b));
                SET_MONEY_AMOUNT(b, amount);
                goto compare;
            }
            if (tb == REB_DECIMAL || tb == REB_PERCENT) {
                deci amount = decimal_to_deci(VAL_DECIMAL(b));
                SET_MONEY_AMOUNT(b, amount);
                goto compare;
            }
            break;

        case REB_WORD:
        case REB_SET_WORD:
        case REB_GET_WORD:
        case REB_LIT_WORD:
        case REB_REFINEMENT:
        case REB_ISSUE:
            if (ANY_WORD(b)) goto compare;
            break;

        case REB_STRING:
        case REB_FILE:
        case REB_EMAIL:
        case REB_URL:
        case REB_TAG:
            if (ANY_STRING(b)) goto compare;
            break;
        }

        if (strictness == 0 || strictness == 1) return 0;
        //if (strictness >= 2)
        fail (Error(RE_INVALID_COMPARE, Type_Of(a), Type_Of(b)));
    }

compare:
    // At this point, both args are of the same datatype.
    if (!(code = Compare_Types[VAL_TYPE_0(a)])) return 0;
    result = code(a, b, strictness);
    if (result < 0) fail (Error(RE_INVALID_COMPARE, Type_Of(a), Type_Of(b)));
    return result;
}


//  EQUAL? < EQUIV? < STRICT-EQUAL? < SAME?

//
//  equal?: native [
//  
//  "Returns TRUE if the values are equal."
//  
//      value1 [opt-any-value!]
//      value2 [opt-any-value!]
//  ]
//
REBNATIVE(equal_q)
{
    if (Compare_Modify_Values(D_ARG(1), D_ARG(2), 0)) return R_TRUE;
    return R_FALSE;
}


//
//  not-equal?: native [
//  
//  "Returns TRUE if the values are not equal."
//  
//      value1 [opt-any-value!]
//      value2 [opt-any-value!]
//  ]
//
REBNATIVE(not_equal_q)
{
    if (Compare_Modify_Values(D_ARG(1), D_ARG(2), 0)) return R_FALSE;
    return R_TRUE;
}


//
//  equiv?: native [
//  
//  "Returns TRUE if the values are equivalent."
//  
//      value1 [opt-any-value!]
//      value2 [opt-any-value!]
//  ]
//
REBNATIVE(equiv_q)
{
    if (Compare_Modify_Values(D_ARG(1), D_ARG(2), 1)) return R_TRUE;
    return R_FALSE;
}


//
//  not-equiv?: native [
//  
//  "Returns TRUE if the values are not equivalent."
//  
//      value1 [opt-any-value!]
//      value2 [opt-any-value!]
//  ]
//
REBNATIVE(not_equiv_q)
{
    if (Compare_Modify_Values(D_ARG(1), D_ARG(2), 1)) return R_FALSE;
    return R_TRUE;
}


//
//  strict-equal?: native [
//  
//  "Returns TRUE if the values are strictly equal."
//  
//      value1 [opt-any-value!]
//      value2 [opt-any-value!]
//  ]
//
REBNATIVE(strict_equal_q)
{
    if (Compare_Modify_Values(D_ARG(1), D_ARG(2), 2)) return R_TRUE;
    return R_FALSE;
}


//
//  strict-not-equal?: native [
//  
//  "Returns TRUE if the values are not strictly equal."
//  
//      value1 [opt-any-value!]
//      value2 [opt-any-value!]
//  ]
//
REBNATIVE(strict_not_equal_q)
{
    if (Compare_Modify_Values(D_ARG(1), D_ARG(2), 2)) return R_FALSE;
    return R_TRUE;
}


//
//  same?: native [
//  
//  "Returns TRUE if the values are identical."
//  
//      value1 [opt-any-value!]
//      value2 [opt-any-value!]
//  ]
//
REBNATIVE(same_q)
{
    if (Compare_Modify_Values(D_ARG(1), D_ARG(2), 3)) return R_TRUE;
    return R_FALSE;
}


//
//  lesser?: native [
//  
//  {Returns TRUE if the first value is less than the second value.}
//  
//      value1 value2
//  ]
//
REBNATIVE(lesser_q)
{
    if (Compare_Modify_Values(D_ARG(1), D_ARG(2), -1)) return R_FALSE;
    return R_TRUE;
}


//
//  lesser-or-equal?: native [
//  
//  {Returns TRUE if the first value is less than or equal to the second value.}
//  
//      value1 value2
//  ]
//
REBNATIVE(lesser_or_equal_q)
{
    if (Compare_Modify_Values(D_ARG(1), D_ARG(2), -2)) return R_FALSE;
    return R_TRUE;
}


//
//  greater?: native [
//  
//  {Returns TRUE if the first value is greater than the second value.}
//  
//      value1 value2
//  ]
//
REBNATIVE(greater_q)
{
    if (Compare_Modify_Values(D_ARG(1), D_ARG(2), -2)) return R_TRUE;
    return R_FALSE;
}


//
//  greater-or-equal?: native [
//  
//  {Returns TRUE if the first value is greater than or equal to the second value.}
//  
//      value1 value2
//  ]
//
REBNATIVE(greater_or_equal_q)
{
    if (Compare_Modify_Values(D_ARG(1), D_ARG(2), -1)) return R_TRUE;
    return R_FALSE;
}


//
//  maximum: native [
//  
//  "Returns the greater of the two values."
//  
//      value1 [any-scalar! date! any-series!]
//      value2 [any-scalar! date! any-series!]
//  ]
//
REBNATIVE(maximum)
{
    if (IS_PAIR(D_ARG(1)) || IS_PAIR(D_ARG(2))) {
        Min_Max_Pair(D_OUT, D_ARG(1), D_ARG(2), TRUE);
    }
    else {
        REBVAL a = *D_ARG(1);
        REBVAL b = *D_ARG(2);
        if (Compare_Modify_Values(&a, &b, -1))
            *D_OUT = *D_ARG(1);
        else
            *D_OUT = *D_ARG(2);
    }
    return R_OUT;
}


//
//  minimum: native [
//  
//  "Returns the lesser of the two values."
//  
//      value1 [any-scalar! date! any-series!]
//      value2 [any-scalar! date! any-series!]
//  ]
//
REBNATIVE(minimum)
{
    if (IS_PAIR(D_ARG(1)) || IS_PAIR(D_ARG(2))) {
        Min_Max_Pair(D_OUT, D_ARG(1), D_ARG(2), FALSE);
    }
    else {
        REBVAL a = *D_ARG(1);
        REBVAL b = *D_ARG(2);

        if (Compare_Modify_Values(&a, &b, -1))
            *D_OUT = *D_ARG(2);
        else
            *D_OUT = *D_ARG(1);
    }
    return R_OUT;
}


//
//  negative?: native [
//  
//  "Returns TRUE if the number is negative."
//  
//      number [any-number! money! time! pair!]
//  ]
//
REBNATIVE(negative_q)
{
    REBVAL zero;
    VAL_INIT_WRITABLE_DEBUG(&zero);

    SET_ZEROED(&zero, VAL_TYPE(D_ARG(1)));

    if (Compare_Modify_Values(D_ARG(1), &zero, -1)) return R_FALSE;
    return R_TRUE;
}


//
//  positive?: native [
//  
//  "Returns TRUE if the value is positive."
//  
//      number [any-number! money! time! pair!]
//  ]
//
REBNATIVE(positive_q)
{
    REBVAL zero;
    VAL_INIT_WRITABLE_DEBUG(&zero);

    SET_ZEROED(&zero, VAL_TYPE(D_ARG(1)));

    if (Compare_Modify_Values(D_ARG(1), &zero, -2)) return R_TRUE;

    return R_FALSE;
}


//
//  zero?: native [
//  
//  {Returns TRUE if the value is zero (for its datatype).}
//  
//      value
//  ]
//
REBNATIVE(zero_q)
{
    enum Reb_Kind type = VAL_TYPE(D_ARG(1));

    if (type >= REB_INTEGER && type <= REB_TIME) {
        REBVAL zero;
        VAL_INIT_WRITABLE_DEBUG(&zero);

        SET_ZEROED(&zero, type);

        if (Compare_Modify_Values(D_ARG(1), &zero, 1)) return R_TRUE;
    }
    return R_FALSE;
}
