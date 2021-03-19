//
//  File: %main.c
//  Summary: "Console application main entry point"
//  Project: "Rebol 3 Interpreter and Run-time (Ren-C branch)"
//  Homepage: https://github.com/metaeducation/ren-c/
//
//=////////////////////////////////////////////////////////////////////////=//
//
// Copyright 2012-2021 Ren-C Open Source Contributors
// Copyright 2012 REBOL Technologies
// REBOL is a trademark of REBOL Technologies
//
// See README.md and CREDITS.md for more information.
//
// Licensed under the Lesser GPL, Version 3.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.gnu.org/licenses/lgpl-3.0.html
//
//=////////////////////////////////////////////////////////////////////////=//
//
// This contains the main() routine, which uses the libRebol API to start up
// an interactive console system for environments that can compile C.
//
// On POSIX systems it uses <termios.h> to implement line editing:
//
// http://pubs.opengroup.org/onlinepubs/7908799/xbd/termios.html
//
// On Windows it uses the Console API:
//
// https://msdn.microsoft.com/en-us/library/ms682087.aspx
//
// Very little work is done in C.  For instance, the command line arguments
// are processed using PARSE by Rebol code that is embedded into the
// executable as compressed bytes.  And the majority of the console behavior
// is defined by Rebol code in %extensions/console (though it has some of
// its own C to handle things like SIGINT for Ctrl-C handling).
//

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef TO_WINDOWS
    #undef _WIN32_WINNT  // https://forum.rebol.info/t/326/4
    #define _WIN32_WINNT 0x0501  // Minimum API target: WinXP
    #define WIN32_LEAN_AND_MEAN  // trim down the Win32 headers
    #include <windows.h>

    #include <shellapi.h>  // for CommandLineToArgvW()
#endif

#include "pstdint.h"  // stdint.h for builds w/pre-C99 compilers--see notes
#include "pstdbool.h"  // stdbool.h for builds w/pre-C99 compilers--see notes

// This file should only use the external API.  However, it can be helpful in
// debug situations to have access to PROBE() and other internal features.
//
#if !defined(DEBUG_MAIN_USING_SYS_CORE)
    #include "rebol.h"  // note: includes pstdint.h and pstdbool.h by default
#else
    #undef IS_ERROR  // windows.h has its own definition of this macro
    #include "sys-core.h"
#endif

#include "reb-c.h"  // provides cast(), UNUSED(), etc.


// Initialization done by rebStartup() is intended to be as basic as possible
// in order to get the Rebol series/values/array functions ready to be run.
// Once that's ready, the rest of the initialization can take advantage of
// a working evaluator.  This includes PARSE to process the command line
// parameters, or PRINT to output boot banners.
//
// This is the %main-startup.reb file bundled with other modules, compressed
// with Gzip and expressed as a C binary literal.  That literal is TRANSCODE'd
// and executed to return the MAIN-STARTUP function, which takes the command
// line arguments as an array of TEXT! and handles things from there.
//
#include "tmp-main-startup.inc"  // generated by %prep-main.reb


#ifdef TO_WINDOWS
    //
    // Most Windows-specific code is expected to be run in extensions (or
    // in the interim, in "devices").  However, it's expected that all Windows
    // code be able to know its `HINSTANCE`.  This is usually passed in a
    // WinMain(), but since we don't use WinMain() in order to be able to
    // act as a console app -or- a GUI app some tricks are needed to capture
    // it, and then export it for other code to use.
    //
    // !!! This is not currently exported via EXTERN_C, because the core was
    // building in a dependency on the host.  This created problems for the
    // libRebol, which needs to be independent of %host-main.c, and may be
    // used with clients that do not have the HINSTANCE easily available.
    // The best idea for exporting it is probably to have those clients who
    // provide it to inject it into the system object as a HANDLE!, so that
    // those extensions which need it have access to it, while not creating
    // problems for those that do not.
    //
    HINSTANCE App_Instance = 0;

    // For why this is done this way with a potential respawning, see the
    // StackOverflow question:
    //
    // "Can one executable be both a console and a GUI application":
    //
    //     http://stackoverflow.com/q/493536/
    //
    void Determine_Hinstance_May_Respawn(WCHAR *this_exe_path) {
        if (GetStdHandle(STD_OUTPUT_HANDLE) == 0) {
            //
            // No console to attach to, we must be the DETACHED_PROCESS which
            // was spawned in the below branch.
            //
            App_Instance = GetModuleHandle(nullptr);
        }
        else {
          #ifdef REB_CORE
            //
            // In "Core" mode, use a console but do not initialize graphics.
            // (stdio redirection works, blinking console window during start)
            //
            App_Instance = cast(HINSTANCE,
                GetWindowLongPtr(GetConsoleWindow(), GWLP_HINSTANCE)
            );
            UNUSED(this_exe_path);
          #else
            //
            // In the "GUI app" mode, stdio redirection doesn't work properly,
            // but no blinking console window during start.
            //
            if (not this_exe_path) { // argc was > 1
                App_Instance = cast(HINSTANCE,
                    GetWindowLongPtr(GetConsoleWindow(), GWLP_HINSTANCE)
                );
            }
            else {
                // Launch child as a DETACHED_PROCESS so that GUI can be
                // initialized, and exit.
                //
                STARTUPINFO startinfo;
                ZeroMemory(&startinfo, sizeof(startinfo));
                startinfo.cb = sizeof(startinfo);

                PROCESS_INFORMATION procinfo;
                if (not CreateProcess(
                    nullptr, // lpApplicationName
                    this_exe_path, // lpCommandLine
                    nullptr, // lpProcessAttributes
                    nullptr, // lpThreadAttributes
                    FALSE, // bInheritHandles
                    CREATE_DEFAULT_ERROR_MODE | DETACHED_PROCESS,
                    nullptr, // lpEnvironment
                    nullptr, // lpCurrentDirectory
                    &startinfo,
                    &procinfo
                )){
                    MessageBox(
                        nullptr, // owner window
                        L"CreateProcess() failed in %host-main.c",
                        this_exe_path, // title
                        MB_ICONEXCLAMATION | MB_OK
                    );
                }

                exit(0);
            }
          #endif
        }
    }
#endif


//=//// MAIN ENTRY POINT //////////////////////////////////////////////////=//
//
// Using a main() entry point for a console program (as opposed to WinMain())
// so we can connect to the console.  See Determine_Hinstance_May_Respawn().
//
int main(int argc, char *argv_ansi[])
{
    // Note: By default, Ctrl-C is not hooked or handled.  This is done by
    // the console extension (%extensions/console).  Halting should not be
    // possible while the mezzanine is loading.

    rebStartup();

    // With interpreter startup done, we want to turn the platform-dependent
    // argument strings into a block of Rebol strings as soon as possible.
    // That way the command line argument processing can be taken care of by
    // PARSE in the MAIN-STARTUP user function, instead of C code!
    //
    REBVAL *argv_block = rebValue("[]");

  #ifdef TO_WINDOWS
    //
    // Were we using WinMain we'd be getting our arguments in Unicode, but
    // since we're using an ordinary main() we do not.  However, this call
    // lets us slip out and pick up the arguments in Unicode form (UTF-16).
    //
    WCHAR **argv_utf16 = CommandLineToArgvW(GetCommandLineW(), &argc);
    UNUSED(argv_ansi);

    Determine_Hinstance_May_Respawn(argc > 1 ? nullptr : argv_utf16[0]);

    int i;
    for (i = 0; i != argc; ++i) {
        rebElide("append", argv_block, rebR(rebTextWide(argv_utf16[i])));
    }
  #else
    // Just take the ANSI C "char*" args...which should ideally be in UTF-8.
    //
    int i = 0;
    for (; i != argc; ++i) {
        rebElide("append", argv_block, rebT(argv_ansi[i]));
    }
  #endif

    // Unzip the Gzip'd compressed startup code (embedded as bytes in a C
    // global variable) to make a BINARY!.  GUNZIP accepts a HANDLE! as input,
    // so pass it in here.
    //
    REBVAL *startup_bin = rebValue(
        "gunzip", rebR(rebHandle(
            m_cast(unsigned char*, &Main_Startup_Code[0]),
            MAIN_STARTUP_SIZE,
            nullptr
        ))
    );

    // The embedded startup data contains the source code for several basic
    // modules that are useful to have built in:
    //
    //   * Compressing and Decompressing .ZIP files
    //   * Encapping and un-Encapping data into an R3 Executable
    //   * Support for HTTP and TLS/HTTPS Protocols (needs Crypt extension)
    //   * Experimental code not yet written as natives
    //
    // The string of code is stylized to look like:
    //
    //     import module [Name: 'Zip ...] [...]
    //     import module [Name: 'Encap ...] [...]
    //     ...
    //     import module [Name: 'Main-Startup ...] [...]
    //     :main-startup
    //
    // There are no top-level SET-WORD!s, and it doesn't leak any declarations
    // into LIB.  The return of the MAIN-STARTUP function as the last item
    // means we get back a usermode function that is ready to process the
    // command line arguments.
    //
    REBVAL *main_startup = rebValue(
        "ensure action! do transcode/where", rebR(startup_bin), "lib"
    );

    // This runs the MAIN-STARTUP, which returns *requests* to execute
    // arbitrary code by way of its return results.  The ENTRAP is thus here
    // to intercept bugs *in MAIN-STARTUP itself*.
    //
    REBVAL *trapped = rebValue(
        "entrap [",  // MAIN-STARTUP action! takes one argument (argv[])
            main_startup, rebR(argv_block),
        "]"
    );
    rebRelease(main_startup);

    if (rebDid("error?", trapped))  // error in MAIN-STARTUP itself
        rebJumps("panic", trapped);  // terminates

    REBVAL *code = rebValue("first", trapped); // entrap's output
    rebRelease(trapped);  // don't need the outer block any more

    // !!! For the moment, the CONSOLE extension does all the work of running
    // usermode code or interpreting exit codes.  This requires significant
    // logic which is reused by the debugger, which ranges from the managing
    // of Ctrl-C enablement and disablement (and how that affects the ability
    // to set unix flags for unblocking file-I/O) to protecting against other
    // kinds of errors.  Hence there is a /PROVOKE refinement to CONSOLE
    // which feeds it an instruction, as if the console gave it to itself.

    REBVAL *result = rebValue("console/provoke", rebR(code));

    int exit_status = rebUnboxInteger(rebR(result));

    const bool clean = false;  // process exiting, not necessary
    rebShutdown(clean);  // Note: debug build runs a clean shutdown anyway

    return exit_status;  // http://stackoverflow.com/q/1101957/
}
