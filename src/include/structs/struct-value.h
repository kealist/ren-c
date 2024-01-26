//
//  File: %struct-value.h
//  Summary: "Value structure defininitions preceding %tmp-internals.h"
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012 REBOL Technologies
// Copyright 2012-2019 Ren-C Open Source Contributors
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information
//
// Licensed under the Lesser GPL, Version 3.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.gnu.org/licenses/lgpl-3.0.html
//
//=////////////////////////////////////////////////////////////////////////=//
//
//=////////////////////////////////////////////////////////////////////////=//
//
//  RELATIVE AND SPECIFIC VALUES (difference enforced in C++ build only)
//
//=////////////////////////////////////////////////////////////////////////=//
//
// A Cell is an equivalent struct layout to to Value, but is allowed to
// have an Action* as its binding.  These relative cells can point to a
// specific Value, but a relative word or array cannot be pointed to by a
// plain Value*.  The Cell-vs-Value distinction is purely commentary
// in the C build, but the C++ build makes Value a type derived from Cell.
//
// Cell exists to help quarantine the bit patterns for relative words into
// the deep-copied-body of the function they are for.  To actually look them
// up, they must be paired with a FRAME! matching the actual instance of the
// running function on the stack they correspond to.  Once made specific,
// a word may then be freely copied into any Value slot.
//
// In addition to ANY-WORD!, an ANY-ARRAY! can also be relative, if it is
// part of the deep-copied function body.  The reason that arrays must be
// relative too is in case they contain relative words.  If they do, then
// recursion into them must carry forward the resolving "specifier" pointer
// to be combined with any relative words that are seen later.
//

#if CPLUSPLUS_11

    // An Atom* is able to hold unstable isotope states.  A separate type
    // is used to avoid propagating the concerns of unstable isotopes to
    // routines that shouldn't have to worry about them.
    //
    struct Atom : public Cell
    {
      #if !defined(NDEBUG)
        Atom() = default;
        ~Atom() {
            assert(
                (this->header.bits & (NODE_FLAG_NODE | NODE_FLAG_CELL))
                or this->header.bits == CELL_MASK_0
            );
        }
      #endif
    };

    struct ValueStruct : public Atom {
      #if !defined(NDEBUG)
        ValueStruct () = default;
        ~ValueStruct () {
            assert(
                (this->header.bits & (NODE_FLAG_NODE | NODE_FLAG_CELL))
                or this->header.bits == CELL_MASK_0
            );
        }
      #endif
    };

    static_assert(
        std::is_standard_layout<struct ValueStruct>::value,
        "C++ REBVAL must match C layout: http://stackoverflow.com/a/7189821/"
    );

    struct Element : public ValueStruct {
      #if !defined(NDEBUG)
        Element () = default;
        ~Element () {
            assert(
                (this->header.bits & (NODE_FLAG_NODE | NODE_FLAG_CELL))
                or this->header.bits == CELL_MASK_0
            );
        }
      #endif
    };
#else
    typedef struct ValueStruct Atom;
    typedef struct ValueStruct Element;
#endif

typedef struct ValueStruct Value;


//=//// VARS and PARAMs ///////////////////////////////////////////////////=//
//
// These are lightweight classes on top of cells that help catch cases of
// testing for flags that only apply if you're sure something is a parameter
// cell or variable cell.
//

#if CPLUSPLUS_11
    struct Param : public REBVAL {};

    INLINE const Param* cast_PAR(const REBVAL *v)
        { return c_cast(Param*, v); }

    INLINE Param* cast_PAR(REBVAL *v)
        { return cast(Param*, v); }
#else
    #define Param REBVAL

    #define cast_PAR(v) (v)
#endif


// Because atoms are supersets of value, you may want to pass an atom to a
// function that writes a value.  But such passing is usually illegal, due
// to wanting to protect functions that only expect stable isotopes from
// getting unstable ones.  So you need to specifically point out that the
// atom is being written into and its contents not heeded.
//
// In the debug build we can give this extra teeth by wiping the contents
// of the atom, to ensure they are not examined.
//
#define Stable_Unchecked(atom) \
    x_cast(Value*, ensure(const Atom*, (atom)))

INLINE REBVAL* Freshen_Cell_Untracked(Cell* v);

#if CPLUSPLUS_11
    template<typename T>
    struct SinkWrapper {
        T* p;

        SinkWrapper() = default;  // or MSVC warns making Option(Sink(Value*))
        SinkWrapper(nullptr_t) : p (nullptr) {}

        template<
            typename U,
            typename std::enable_if<
                std::is_base_of<U,T>::value  // e.g. pass Atom to Sink(Element)
            >::type* = nullptr
        >
        SinkWrapper(U* u) : p (u_cast(T*, u)) {
            /* Init_Unreadable(p); */
        }

        template<
            typename U,
            typename std::enable_if<
                std::is_base_of<U,T>::value  // e.g. pass Atom to Sink(Element)
            >::type* = nullptr
        >
        SinkWrapper(SinkWrapper<U> u) : p (u_cast(T*, u.p)) {
        }

        operator bool () const { return p != nullptr; }

        operator T* () const { return p; }

        operator copy_const_t<Node,T>* () const { return p; }

        explicit operator copy_const_t<Byte,T>* () const
          { return reinterpret_cast<copy_const_t<Byte,T>*>(p); }

        T* operator->() const { return p; }
    };

    #define Sink(T) SinkWrapper<std::remove_pointer<T>::type>
    #define Need(T) SinkWrapper<std::remove_pointer<T>::type>

    template<>
    struct c_cast_helper<Byte*, Sink(Value*) const&> {
        typedef Byte* type;
    };
#else
    #define Sink(T) T
    #define Need(T) T
#endif


//=//// EXTANT STACK POINTERS /////////////////////////////////////////////=//
//
// See %sys-stack.h for a deeper explanation.  This has to be declared in
// order to put in one of NoQuote(const Cell*)s implicit constructors.  Because
// having the StackValue(*) have a user-defined conversion to REBVAL* won't
// get that...and you can't convert to both REBVAL* and NoQuote(const Cell*) as
// that would be ambiguous.
//
// Even with this definition, the intersecting needs of DEBUG_CHECK_CASTS and
// DEBUG_EXTANT_STACK_POINTERS means there will be some cases where distinct
// overloads of REBVAL* vs. NoQuote(const Cell*) will wind up being ambiguous.
// For instance, VAL_DECIMAL(StackValue(*)) can't tell which checked overload
// to use.  Then you have to cast, e.g. VAL_DECIMAL(cast(Value*, stackval)).
//
#if (! DEBUG_EXTANT_STACK_POINTERS)
    #define StackValue(p) REBVAL*
#else
    struct StackValuePointer;
    #define StackValue(p) StackValuePointer
#endif
