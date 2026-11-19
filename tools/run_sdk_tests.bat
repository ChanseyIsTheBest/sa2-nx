@echo off
rem banjo-nx: builds and runs the SDK's unit tests of sa2-nx's CPU rounds of 2026-10-12 (sdk\tests\*.cpp, from sasrt-nx
rem and sa2-nx) on the PC with clang. Each test prints its own result and exits non-zero on a difference.
rem   tools\run_sdk_tests.bat [name filter]
rem fp_any_mode_test runs twice: the helpers (must report 0 differences) and its negative control (the plain
rem operations in the helpers' place: must report differences, which shows the inputs reach the cases that matter).
setlocal enabledelayedexpansion
call "%~dp0pc_env.bat" || exit /b 1
cd /d "%~dp0.."
if not exist out\tests mkdir out\tests
rem Linked as the CMake build links (this clang has no compiler-rt for the MSVC target): no default libraries, the
rem MSVC runtime named by the object.
set "CXX=%LLVM_BIN%/clang++.exe --target=x86_64-pc-windows-msvc -O2 -std=c++23 -ffp-contract=off -march=x86-64-v2 -D_DLL -D_MT -Xclang --dependent-lib=msvcrt -nostartfiles -nostdlib -fuse-ld=lld-link -lkernel32.lib -loldnames.lib"
set "INC=-Isdk/include -Isdk/thirdparty/simde -Isdk/thirdparty/simde/simde -Iapp/out/pc-me/rexglue-sdk/include"
set FAILED=0
for %%T in (fp_any_mode_test rotate_word_test signal_skip_idle_test vec_byteswap_test vmx_helpers_neon_test vmx_small_helpers_test neon_shuffles_test fcti_copy_test float_loads_test vec_unaligned_test memcalls_test fp_sign_ops_test single_floats_test pmull_fingerprint_test vec_lane_test) do (
  set "RUN=1"
  if not "%~1"=="" (echo %%T | findstr /i "%~1" >nul || set "RUN=0")
  if "!RUN!"=="1" (
    echo == %%T: building
    %CXX% %INC% sdk/tests/%%T.cpp -o out\tests\%%T.exe
    if errorlevel 1 (
      echo BUILD FAILED
      set /a FAILED+=1
    ) else (
      if "%%T"=="fp_any_mode_test" (
        echo == %%T 30000000
        out\tests\%%T.exe 30000000 || (echo FAILED & set /a FAILED+=1)
        echo == %%T 2000000 control
        out\tests\%%T.exe 2000000 control || (echo FAILED & set /a FAILED+=1)
      ) else if "%%T"=="signal_skip_idle_test" (
        echo == %%T 8
        out\tests\%%T.exe 8 || (echo FAILED & set /a FAILED+=1)
      ) else (
        echo == %%T
        out\tests\%%T.exe || (echo FAILED & set /a FAILED+=1)
      )
    )
  )
)
echo == done: %FAILED% failed
exit /b %FAILED%
