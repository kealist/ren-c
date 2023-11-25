// %cell-array.h

#define EMPTY_BLOCK \
    Root_Empty_Block

#define EMPTY_ARRAY \
    PG_Empty_Array // Note: initialized from Cell_Array(Root_Empty_Block)


INLINE bool Any_Arraylike(NoQuote(const Cell*) v) {
    // called by core code, sacrifice READABLE() checks
    if (Any_Array_Kind(Cell_Heart_Unchecked(v)))
        return true;
    if (not Any_Sequence_Kind(Cell_Heart_Unchecked(v)))
        return false;
    if (Not_Cell_Flag_Unchecked(v, FIRST_IS_NODE))
        return false;
    const Node* node1 = Cell_Node1(v);
    if (Is_Node_A_Cell(node1))
        return true;  // Cell_Array_At() works, but Cell_Array() won't work!
    return Series_Flavor(u_cast(const Series*, node1)) == FLAVOR_ARRAY;
}

INLINE const Array* Cell_Array(NoQuote(const Cell*) v) {
    assert(Any_Arraylike(v));
    assert(Is_Node_A_Stub(Cell_Node1(v)));  // not a pairing arraylike!

    const Array* a = cast(Array*, Cell_Node1(v));
    if (Get_Series_Flag(a, INACCESSIBLE))
        fail (Error_Series_Data_Freed_Raw());
    return a;
}

#define Cell_Array_Ensure_Mutable(v) \
    m_cast(Array*, Cell_Array(Ensure_Mutable(v)))

#define Cell_Array_Known_Mutable(v) \
    m_cast(Array*, Cell_Array(Known_Mutable(v)))


// These array operations take the index position into account.  The use
// of the word AT with a missing index is a hint that the index is coming
// from the VAL_INDEX() of the value itself.
//
// IMPORTANT: This routine will trigger a failure if the array index is out
// of bounds of the data.  If a function can deal with such out of bounds
// arrays meaningfully, it should work with VAL_INDEX_UNBOUNDED().
//
INLINE const Cell* Cell_Array_Len_At(
    Option(Length*) len_at_out,
    NoQuote(const Cell*) v
){
    const Node* node = Cell_Node1(v);
    if (Is_Node_A_Cell(node)) {
        assert(Any_Sequence_Kind(Cell_Heart(v)));
        assert(VAL_INDEX_RAW(v) == 0);
        if (len_at_out)
            *unwrap(len_at_out) = PAIRING_LEN;
        return c_cast(Cell*, node);
    }
    const Array* arr = c_cast(Array*, node);
    REBIDX i = VAL_INDEX_RAW(v);  // Cell_Array() already checks it's series
    Length len = Array_Len(arr);
    if (i < 0 or i > cast(REBIDX, len))
        fail (Error_Index_Out_Of_Range_Raw());
    if (len_at_out)  // inlining should remove this if() for Cell_Array_At()
        *unwrap(len_at_out) = len - i;
    return Array_At(arr, i);
}

INLINE const Cell* Cell_Array_At(
    Option(const Cell**) tail_out,
    NoQuote(const Cell*) v
){
    const Node* node = Cell_Node1(v);
    if (Is_Node_A_Cell(node)) {
        assert(Any_Sequence_Kind(Cell_Heart(v)));
        const Cell* cell = c_cast(Cell*, node);
        if (tail_out)
            *unwrap(tail_out) = Pairing_Tail(cell);
        return cell;
    }
    const Array* arr = c_cast(Array*, node);
    REBIDX i = VAL_INDEX_RAW(v);  // Cell_Array() already checks it's arraylike
    Length len = Array_Len(arr);
    if (i < 0 or i > cast(REBIDX, len))
        fail (Error_Index_Out_Of_Range_Raw());
    const Cell* at = Array_At(arr, i);
    if (tail_out)  // inlining should remove this if() for no tail
        *unwrap(tail_out) = at + (len - i);
    return at;
}

INLINE const Cell* Cell_Array_Item_At(NoQuote(const Cell*) v) {
    const Cell* tail;
    const Cell* item = Cell_Array_At(&tail, v);
    assert(item != tail);  // should be a valid value
    return item;
}


#define Cell_Array_At_Ensure_Mutable(tail_out,v) \
    m_cast(Cell*, Cell_Array_At((tail_out), Ensure_Mutable(v)))

#define Cell_Array_At_Known_Mutable(tail_out,v) \
    m_cast(Cell*, Cell_Array_At((tail_out), Known_Mutable(v)))


// !!! R3-Alpha introduced concepts of immutable series with PROTECT, but
// did not consider the protected status to apply to binding.  Ren-C added
// more notions of immutability (const, holds, locking/freezing) and enforces
// it at compile-time...which caught many bugs.  But being able to bind
// "immutable" data was mechanically required by R3-Alpha for efficiency...so
// new answers will be needed.  See Virtual_Bind_Deep_To_New_Context() for
// some of the thinking on this topic.  Until it's solved, binding-related
// calls to this function get mutable access on non-mutable series.  :-/
//
#define Cell_Array_At_Mutable_Hack(tail_out,v) \
    m_cast(Cell*, Cell_Array_At((tail_out), (v)))


//=//// ANY-ARRAY! INITIALIZER HELPERS ////////////////////////////////////=//
//
// Declaring as inline with type signature ensures you use a Array* to
// initialize, and the C++ build can also validate managed consistent w/const.

INLINE REBVAL *Init_Array_Cell_At_Core(
    Cell* out,
    enum Reb_Kind kind,
    const_if_c Array* array,
    REBLEN index,
    Array* binding
){
    return Init_Series_Cell_At_Core(
        out,
        kind,
        Force_Series_Managed_Core(array),
        index,
        binding
    );
}

#if CPLUSPLUS_11
    INLINE REBVAL *Init_Array_Cell_At_Core(
        Cell* out,
        enum Reb_Kind kind,
        const Array* array,  // all const arrays should be already managed
        REBLEN index,
        Array* binding
    ){
        return Init_Series_Cell_At_Core(out, kind, array, index, binding);
    }
#endif

#define Init_Array_Cell_At(v,t,a,i) \
    Init_Array_Cell_At_Core((v), (t), (a), (i), UNBOUND)

#define Init_Array_Cell(v,t,a) \
    Init_Array_Cell_At((v), (t), (a), 0)

#define Init_Block(v,s)     Init_Array_Cell((v), REB_BLOCK, (s))
#define Init_Group(v,s)     Init_Array_Cell((v), REB_GROUP, (s))


INLINE Cell* Init_Relative_Block_At(
    Cell* out,
    Action* action,  // action to which array has relative bindings
    Array* array,
    REBLEN index
){
    Reset_Unquoted_Header_Untracked(out, CELL_MASK_BLOCK);
    Init_Cell_Node1(out, array);
    VAL_INDEX_RAW(out) = index;
    INIT_SPECIFIER(out, action);
    return out;
}

#define Init_Relative_Block(out,action,array) \
    Init_Relative_Block_At((out), (action), (array), 0)


//=//// "PACKS" (BLOCK! Isotopes) /////////////////////////////////////////=//
//
// BLOCK! isotopes are exploited as a mechanism for bundling values in a way
// that they can be passed around as a single value.  They are leveraged in
// particular for multi-return, because a SET-WORD! will unpack only the
// first item, while a SET-BLOCK! will unpack others.
//
//      >> pack [<a> <b>]
//      == ~['<a> '<b>]~  ; isotope
//
//      >> x: pack [<a> <b>]
//      == <a>
//
//      >> [x y]: pack [<a> <b>]
//      == <a>
//
//      >> x
//      == <a>
//
//      >> y
//      == <b>
//

INLINE Value(*) Init_Pack_Untracked(Atom(*) out, Array* a) {
    Init_Block(out, a);
    QUOTE_BYTE(out) = ISOTOPE_0;
    return cast(Value(*), out);  // Note: Is_Isotope_Unstable(out)
}

#define Init_Pack(out,a) \
    TRACK(Init_Pack_Untracked((out), (a)))


//=//// "NIHIL" (empty BLOCK! Isotope Pack, ~[]~) /////////////////////////=//
//
// This unstable isotope is used in situations that want to convey a full
// absence of values (e.g. ELIDE).  It can't be used in assignments, and if
// the evaluator encounters one in an interstitial context it will be
// vaporized.  It is sensibly represented as a parameter pack of length 0.
//

#define Init_Nihil_Untracked(out) \
    Init_Pack_Untracked((out), EMPTY_ARRAY)

#define Init_Nihil(out) \
    TRACK(Init_Nihil_Untracked(out))

INLINE bool Is_Nihil(Atom(const*) v) {
    if (not Is_Pack(v))
        return false;
    const Cell* tail;
    const Cell* at = Cell_Array_At(&tail, v);
    return tail == at;
}

INLINE bool Is_Meta_Of_Nihil(const Cell* v) {
    if (not Is_Meta_Of_Pack(v))
        return false;
    const Cell* tail;
    const Cell* at = Cell_Array_At(&tail, v);
    return tail == at;
}


//=//// "SPLICES" (GROUP! Isotopes) ///////////////////////////////////////=//
//
// Group isotopes are understood by routines like APPEND/INSERT/CHANGE to
// mean that you intend to splice their content (the default is to append
// as-is, which is changed from Rebol2/Red).  The typical way of making these
// isotopes is the SPREAD function.
//
//    >> append [a b c] [d e]
//    == [a b c] [d e]
//
//    >> spread [d e]
//    == ~(d e)~  ; isotope
//
//    >> append [a b c] ~(d e)~
//    == [a b c d e]
//

INLINE Value(*) Splicify(Value(*) v) {
    assert(Any_Array(v) and QUOTE_BYTE(v) == UNQUOTED_1);
    QUOTE_BYTE(v) = ISOTOPE_0;
    HEART_BYTE(v) = REB_GROUP;
    return v;
}

INLINE Value(*) Init_Splice_Untracked(Value(*) out, Array* a) {
    Init_Group(out, a);
    QUOTE_BYTE(out) = ISOTOPE_0;
    return out;
}

#define Init_Splice(out,a) \
    TRACK(Init_Splice_Untracked((out), (a)))