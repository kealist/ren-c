//
//  File: %reb-event.h
//  Summary: "REBOL event definitions"
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

// Note: size must be 12 bytes on 32-bit or 16 on 64-bit!

#pragma pack(4)
struct Reb_Event {
    u8  type;       // event id (mouse-move, mouse-button, etc)
    u8  flags;      // special flags
    u8  win;        // window id
    u8  model;      // port, object, gui, callback
    u32 data;       // an x/y position or keycode (raw/decoded)
    union {
        REBREQ *req;    // request (for device events)
        REBSER *ser;      // port or object
    } eventee;              // !!! REVIEW: Not always "sender"?  The name is
                            // "bad" (?) but at least unique, making it easy
                            // to change.  See also rebol_devreq->requestee
};
#pragma pack()

// !!! REBEVT might be better-off as a 16/32 byte structure instead of the
// "uneven" sized payload, and then it could carry a whole REBVAL of info
typedef struct Reb_Event REBEVT;

// Special event flags:

enum {
    EVF_COPIED,     // event data has been copied
    EVF_HAS_XY,     // map-event will work on it
    EVF_DOUBLE,     // double click detected
    EVF_CONTROL,
    EVF_SHIFT,
    EVF_MAX
};


// Event port data model

enum {
    EVM_DEVICE,     // I/O request holds the port pointer
    EVM_PORT,       // event holds port pointer
    EVM_OBJECT,     // event holds object context pointer
    EVM_GUI,        // GUI event uses system/view/event/port
    EVM_CALLBACK,   // Callback event uses system/ports/callback port
    EVM_MAX
};

// Special messages
#define WM_DNS (WM_USER+100)
