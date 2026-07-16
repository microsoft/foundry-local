# Copyright (c) Microsoft. All rights reserved.
#
# Splits debug symbols from the foundry_local shared library for RelWithDebInfo
# builds on Linux and macOS.
#
# Linux:   objcopy --only-keep-debug + strip --strip-debug +
#          objcopy --add-gnu-debuglink wires the stripped .so back to its .dbg
#          companion. GDB/LLDB find the .dbg automatically via the GNU
#          debuglink section — no user configuration needed.
#
# macOS:   dsymutil harvests DWARF from the .o debug maps into a .dSYM bundle,
#          then strip -S removes the N_OSO stab entries from the .dylib.
#          LLDB/Xcode find the .dSYM automatically when it sits in the same
#          directory as the .dylib.
#
#          Note: without this step the current macOS build has no persistent
#          debug info — DWARF lives only in .o files that are gone after a
#          clean or on any consumer machine. dsymutil is required.
#
# The split happens at build time so local developers get the same layout as CI
# without any manual steps; GDB/LLDB discover the symbols automatically.
#
# This is a no-op for:
#   Debug   — symbols are intentionally embedded, stripping defeats the purpose.
#   Release — compiled without -g, nothing to extract.

if(NOT (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE))
    return()
endif()

# Single-config generators (Ninja/Makefile) set CMAKE_BUILD_TYPE at configure
# time, which is how both the Linux and macOS CI agents invoke build.py. Only
# wire up the split for RelWithDebInfo.
if(NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    return()
endif()

# --------------------------------------------------------------------------
# macOS: dsymutil → .dSYM bundle, then strip -S
# --------------------------------------------------------------------------
if(APPLE)
    find_program(DSYMUTIL dsymutil)
    if(NOT DSYMUTIL)
        message(WARNING
            "dsymutil not found — dSYM generation disabled for RelWithDebInfo. "
            "Install Xcode Command Line Tools to enable debug symbol extraction.")
        return()
    endif()

    # dsymutil must run BEFORE strip: it reads the N_OSO stab entries that
    # point to .o files to harvest their DWARF. strip -S then removes those
    # stab entries from the .dylib, leaving a clean production binary.
    add_custom_command(TARGET foundry_local POST_BUILD
        COMMAND "${DSYMUTIL}"
            "$<TARGET_FILE:foundry_local>"
            -o "$<TARGET_FILE:foundry_local>.dSYM"
        COMMAND strip
            -S
            "$<TARGET_FILE:foundry_local>"
        COMMENT "Generating dSYM bundle and stripping debug map from libfoundry_local.dylib"
        VERBATIM
    )
    message(STATUS "RelWithDebInfo: dSYM extraction + strip configured for libfoundry_local.dylib")

# --------------------------------------------------------------------------
# Linux: objcopy --only-keep-debug + strip --strip-debug + --add-gnu-debuglink
# --------------------------------------------------------------------------
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Prefer the toolchain's objcopy/strip for cross-compilation correctness
    # (CMAKE_OBJCOPY / CMAKE_STRIP are set by CMake when it detects the
    # compiler). Fall back to find_program for environments that don't have
    # these variables populated.
    set(_objcopy "${CMAKE_OBJCOPY}")
    if(NOT _objcopy)
        find_program(_objcopy objcopy)
    endif()

    set(_strip "${CMAKE_STRIP}")
    if(NOT _strip)
        find_program(_strip strip)
    endif()

    if(NOT _objcopy OR NOT _strip)
        message(WARNING
            "objcopy/strip not found — debug symbol splitting disabled for RelWithDebInfo. "
            "Install binutils to enable symbol extraction.")
        return()
    endif()

    add_custom_command(TARGET foundry_local POST_BUILD
        # 1. Extract DWARF sections into a companion .dbg file.
        COMMAND "${_objcopy}"
            --only-keep-debug
            "$<TARGET_FILE:foundry_local>"
            "$<TARGET_FILE:foundry_local>.dbg"
        # 2. Strip DWARF from the .so. --strip-debug keeps the exported symbol
        #    table (required for dynamic linking) but removes .debug_* sections.
        COMMAND "${_strip}"
            --strip-debug
            "$<TARGET_FILE:foundry_local>"
        # 3. Embed a GNU debuglink section so GDB/LLDB auto-discover the .dbg
        #    file. The debuglink stores the basename; GDB searches the same
        #    directory as the .so first, so colocating them is sufficient.
        COMMAND "${_objcopy}"
            "--add-gnu-debuglink=$<TARGET_FILE:foundry_local>.dbg"
            "$<TARGET_FILE:foundry_local>"
        COMMENT "Splitting debug symbols into libfoundry_local.so.dbg"
        VERBATIM
    )
    message(STATUS "RelWithDebInfo: debug symbol splitting configured for libfoundry_local.so")
endif()
