# renut-ultimate (2026-11-20, port 193): TFU-nx's tfu_build_stamp.cmake (via S4ep1-nx, github.com/FurryLilManChild/
# S4ep1-nx, GPL-3.0). Writes the build stamp header on every build (cmake -DOUT=<header> -DROOT=<repo> -DKIND=<pc|switch>
# -DNAME=<game> -P build_stamp.cmake); the file is rewritten only when its text changes, so only src/build_stamp.cpp
# recompiles then. The stamp: the game, the kind and the commit ("+changes" when the tree has uncommitted edits). TFU's
# also had the build time; left out here, so a build with nothing changed relinks nothing (the Switch LTO link takes
# minutes). It is the first line of rex_stderr.log, rex_profile.log and every crash report
# (sdk/src/ui/switch_crash_hooks.c), and the app logs it at start, so each log is tied to its NRO and ELF.
execute_process(COMMAND git -C "${ROOT}" rev-parse --short=10 HEAD
                OUTPUT_VARIABLE _commit OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _r1)
if(NOT _r1 EQUAL 0 OR _commit STREQUAL "")
    set(_commit "no-git")
endif()
execute_process(COMMAND git -C "${ROOT}" status --porcelain --untracked-files=no
                OUTPUT_VARIABLE _dirty OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _dirty STREQUAL "")
    string(APPEND _commit "+changes")
endif()
set(_text "#define RENUT_BUILD_STAMP \"${NAME} ${KIND} build ${_commit}\"\n")
set(_old "")
if(EXISTS "${OUT}")
    file(READ "${OUT}" _old)
endif()
if(NOT _old STREQUAL _text)
    file(WRITE "${OUT}" "${_text}")
endif()
