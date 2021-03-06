//
//  File: %s-make.c
//  Summary: "binary and unicode string support"
//  Section: strings
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2017 Rebol Open Source Contributors
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

#include "sys-core.h"


//
//  Make_Binary: C
//
// Make a binary string series. For byte, C, and UTF8 strings.
// Add 1 extra for terminator.
//
REBSER *Make_Binary(REBCNT length)
{
    REBSER *series = Make_Series(length + 1, sizeof(REBYTE));

    // !!! Clients seem to have different expectations of if `length` is
    // total capacity (and the binary should be empty) or actually is
    // specifically being preallocated at a fixed length.  Until this
    // is straightened out, terminate for both possibilities.

    BIN_HEAD(series)[length] = 0;
    TERM_SEQUENCE(series);
    return series;
}


//
//  Make_Unicode: C
//
// Make a unicode string series. Used for internal strings.
// Add 1 extra for terminator.
//
REBSER *Make_Unicode(REBCNT length)
{
    REBSER *series = Make_Series(length + 1, sizeof(REBUNI));

    // !!! Clients seem to have different expectations of if `length` is
    // total capacity (and the binary should be empty) or actually is
    // specifically being preallocated at a fixed length.  Until this
    // is straightened out, terminate for both possibilities.

    UNI_HEAD(series)[length] = 0;
    TERM_SEQUENCE(series);
    return series;
}


//
//  Copy_Bytes: C
//
// Create a string series from the given bytes.
// Source is always latin-1 valid. Result is always 8bit.
//
REBSER *Copy_Bytes(const REBYTE *src, REBINT len)
{
    if (len < 0)
        len = LEN_BYTES(src);

    REBSER *dst = Make_Binary(len);
    memcpy(BIN_HEAD(dst), src, len);
    TERM_SEQUENCE_LEN(dst, len);

    return dst;
}


//
//  Insert_Char: C
//
// Insert a unicode char into a string.
//
void Insert_Char(REBSER *dst, REBCNT index, REBCNT chr)
{
    if (index > SER_LEN(dst))
        index = SER_LEN(dst);
    Expand_Series(dst, index, 1);
    SET_ANY_CHAR(dst, index, chr);
}


//
//  Copy_String_At_Len: C
//
// !!! With UTF-8 Everywhere, copying strings will still be distinct from
// other series due to the length being counted in characters and not
// units of the series width.
//
REBSER *Copy_String_At_Len(const RELVAL *src, REBINT limit)
{
    REBCNT length_limit;
    REBSIZ size = VAL_SIZE_LIMIT_AT(&length_limit, src, limit);
    assert(length_limit * 2 == size); // !!! Temporary

    REBSER *dst = Make_Unicode(size / 2);
    memcpy(AS_REBUNI(UNI_AT(dst, 0)), VAL_UNI_AT(src), size);
    TERM_SEQUENCE_LEN(dst, length_limit);

    return dst;
}


//
//  Append_Unencoded_Len: C
//
// Append unencoded data to a byte string, using plain memcpy().  If dst is
// NULL, a new byte-sized series will be created and returned.
//
REBSER *Append_Unencoded_Len(REBSER *dst, const char *src, REBCNT len)
{
    assert(BYTE_SIZE(dst));

    REBCNT tail;
    if (dst == NULL) {
        dst = Make_Binary(len);
        tail = 0;
    }
    else {
        tail = SER_LEN(dst);
        EXPAND_SERIES_TAIL(dst, len);
    }

    memcpy(BIN_AT(dst, tail), src, len);
    TERM_SEQUENCE(dst);
    return dst;
}


//
//  Append_Unencoded: C
//
// Append_Unencoded_Len() variant that looks for a terminating 0 byte to
// determine the length.
//
// !!! Should be in a header file so it can be inlined.
//
REBSER *Append_Unencoded(REBSER *dst, const char *src)
{
    return Append_Unencoded_Len(dst, src, strlen(src));
}


//
//  Append_Codepoint: C
//
// Append a non-encoded character to a string.
//
REBSER *Append_Codepoint(REBSER *dst, REBUNI codepoint)
{
    assert(SER_WIDE(dst) == sizeof(REBUNI)); // invariant for "Latin1 Nowhere"

    REBCNT tail = SER_LEN(dst);
    EXPAND_SERIES_TAIL(dst, 1);

    REBCHR(*) cp = UNI_AT(dst, tail);
    cp = WRITE_CHR(cp, codepoint);
    cp = WRITE_CHR(cp, '\0'); // should always be capacity for terminator

    return dst;
}


//
//  Append_Utf8_Codepoint: C
//
// Encode a codepoint onto a UTF-8 binary series.
//
REBSER *Append_Utf8_Codepoint(REBSER *dst, uint32_t codepoint)
{
    assert(SER_WIDE(dst) == sizeof(REBYTE));

    REBCNT tail = SER_LEN(dst);
    EXPAND_SERIES_TAIL(dst, 4); // !!! Conservative, assume long codepoint
    tail += Encode_UTF8_Char(BIN_AT(dst, tail), codepoint); // 1 to 4 bytes
    TERM_BIN_LEN(dst, tail);
    return dst;
}


//
//  Make_Series_Codepoint: C
//
// Create a string that holds a single codepoint.
//
REBSER *Make_Series_Codepoint(REBCNT codepoint)
{
    assert(codepoint < (1 << 16));

    REBSER *out = Make_Unicode(1);
    *UNI_HEAD(out) = codepoint;
    TERM_UNI_LEN(out, 1);

    return out;
}


//
//  Append_Utf8_Utf8: C
//
// Append a UTF8 byte series to a UTF8 binary.  Terminates.
//
// !!! Currently does the same thing as Append_Unencoded_Len.  Should it
// check the bytes to make sure they're actually UTF8?
//
void Append_Utf8_Utf8(REBSER *dst, const char *utf8, size_t size)
{
    Append_Unencoded_Len(dst, utf8, size);
}


//
//  Append_Utf8_String: C
//
// Append a partial string to a UTF-8 binary series.
//
// !!! Used only with mold series at the moment.
//
void Append_Utf8_String(REBSER *dst, const RELVAL *src, REBCNT length_limit)
{
    assert(
        SER_WIDE(dst) == sizeof(REBYTE)
        && SER_WIDE(VAL_SERIES(src)) == sizeof(REBUNI)
    );

    REBSIZ offset;
    REBSIZ size;
    REBSER *temp = Temp_UTF8_At_Managed(&offset, &size, src, length_limit);

    REBCNT tail = SER_LEN(dst);
    Expand_Series(dst, tail, size); // tail changed too

    memcpy(BIN_AT(dst, tail), BIN_AT(temp, offset), size);
}


//
//  Append_Int: C
//
// Append an integer string.
//
void Append_Int(REBSER *dst, REBINT num)
{
    REBYTE buf[32];

    Form_Int(buf, num);
    Append_Unencoded(dst, s_cast(buf));
}


//
//  Append_Int_Pad: C
//
// Append an integer string.
//
void Append_Int_Pad(REBSER *dst, REBINT num, REBINT digs)
{
    REBYTE buf[32];
    if (digs > 0)
        Form_Int_Pad(buf, num, digs, -digs, '0');
    else
        Form_Int_Pad(buf, num, -digs, digs, '0');

    Append_Unencoded(dst, s_cast(buf));
}



//
//  Append_UTF8_May_Fail: C
//
// Append UTF-8 data to a series underlying an ANY-STRING!.
//
// `dst = NULL` means make a new string.
//
REBSER *Append_UTF8_May_Fail(
    REBSER *dst,
    const char *utf8,
    size_t size,
    REBOOL crlf_to_lf
){
    // This routine does not just append bytes blindly because:
    //
    // * We want to check for invalid codepoints (this can be called with
    //   arbitrary outside data from the API.
    // * It's needed to know how many characters (length) are in the series,
    //   not just how many bytes.  The higher level concept of "length" gets
    //   stored in the series LINK() field.
    // * In the future, some operations will be accelerated by knowing that
    //   a string only contains ASCII codepoints.

    REBSER *temp = BUF_UTF8; // buffer is Unicode width

    Resize_Series(temp, size + 1); // needs at most this many unicode chars

    REBUNI *up = UNI_HEAD(temp);
    const REBYTE *src = cb_cast(utf8);

    REBOOL all_ascii = TRUE;

    REBCNT num_codepoints = 0;

    REBSIZ bytes_left = size; // see remarks on Back_Scan_UTF8_Char's 3rd arg
    for (; bytes_left > 0; --bytes_left, ++src) {
        REBUNI ch = *src;
        if (ch >= 0x80) {
            src = Back_Scan_UTF8_Char(&ch, src, &bytes_left);
            if (src == NULL)
                fail (Error_Bad_Utf8_Raw());

            all_ascii = FALSE;
        }
        else if (ch == CR && crlf_to_lf) {
            if (src[1] == LF)
                continue; // skip the CR, do the decrement and get the LF
            ch = LF;
        }

        ++num_codepoints;
        *up++ = ch;
    }

    up = UNI_HEAD(temp);

    REBCNT old_len;
    if (dst == NULL) {
        dst = Make_Unicode(num_codepoints);
        old_len = 0;
    }
    else {
        old_len = SER_LEN(dst);
        EXPAND_SERIES_TAIL(dst, num_codepoints);
    }

    REBUNI *dp = AS_REBUNI(UNI_AT(dst, old_len));
    SET_SERIES_LEN(dst, old_len + num_codepoints); // counted down to 0 below

    for (; num_codepoints > 0; --num_codepoints)
        *dp++ = *up++;

    *dp = '\0';

    UNUSED(all_ascii);

    return dst;
}


//
//  Join_Binary: C
//
// Join a binary from component values for use in standard
// actions like make, insert, or append.
// limit: maximum number of values to process
// limit < 0 means no limit
//
// WARNING: returns BYTE_BUF, not a copy!
//
REBSER *Join_Binary(const REBVAL *blk, REBINT limit)
{
    REBSER *series = BYTE_BUF;

    REBCNT tail = 0;

    if (limit < 0)
        limit = VAL_LEN_AT(blk);

    SET_SERIES_LEN(series, 0);

    RELVAL *val;
    for (val = VAL_ARRAY_AT(blk); limit > 0; val++, limit--) {
        switch (VAL_TYPE(val)) {
        case REB_INTEGER:
            if (VAL_INT64(val) > 255 || VAL_INT64(val) < 0)
                fail (Error_Out_Of_Range(KNOWN(val)));
            EXPAND_SERIES_TAIL(series, 1);
            *BIN_AT(series, tail) = (REBYTE)VAL_INT32(val);
            break;

        case REB_BINARY: {
            REBCNT len = VAL_LEN_AT(val);
            EXPAND_SERIES_TAIL(series, len);
            memcpy(BIN_AT(series, tail), VAL_BIN_AT(val), len);
            break; }

        case REB_TEXT:
        case REB_FILE:
        case REB_EMAIL:
        case REB_URL:
        case REB_TAG: {
            REBCNT val_len = VAL_LEN_AT(val);
            size_t val_size = Size_As_UTF8(VAL_UNI_AT(val), val_len);

            EXPAND_SERIES_TAIL(series, val_size);
            SET_SERIES_LEN(
                series,
                tail + Encode_UTF8(
                    BIN_AT(series, tail),
                    val_size,
                    VAL_UNI_AT(val),
                    &val_len
                )
            );
            break; }

        case REB_CHAR: {
            EXPAND_SERIES_TAIL(series, 6);
            REBCNT len =
                Encode_UTF8_Char(BIN_AT(series, tail), VAL_CHAR(val));
            SET_SERIES_LEN(series, tail + len);
            break; }

        default:
            fail (Error_Invalid_Core(val, VAL_SPECIFIER(blk)));
        }

        tail = SER_LEN(series);
    }

    *BIN_AT(series, tail) = 0;

    return series;  // SHARED FORM SERIES!
}
