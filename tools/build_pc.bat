@echo off
rem Configure + build the Windows (PC) app against the port SDK (katamari-nx\sdk).
rem Usage: tools\build_pc.bat [extra cmake --build args]
rem   BUILD_DIR (env) defaults to app\out\pc; REXSDK_DIR defaults to katamari-nx\sdk.
rem   RENUT (env) = ON builds Renut's renderer in (BANJO_RENUT_RENDERER); default OFF until its hooks are at
rem   Beautiful Katamari's addresses (docs/katamari-notes.md). HOOKS (env) = 1 turns those hooks on (KATAMARI_RENUT_HOOKS).
call "%~dp0pc_env.bat" || exit /b 1
set "ROOT=%~dp0.."
if "%BUILD_DIR%"=="" set "BUILD_DIR=%ROOT%\app\out\pc"
if "%REXSDK_DIR%"=="" set "REXSDK_DIR=%ROOT%\sdk"
if "%RENUT%"=="" set "RENUT=OFF"
rem renut-ultimate (2026-11-20, port 197): a fresh clone has no app\generated: its rexglue.cmake, which the app's
rem configure includes, is written by code generation (app/.gitignore keeps the folder out of the repository), so the
rem configure stopped at its include. The generated tree is made first, as docs/building.md section 2 says: the code
rem generator (tools\codegen.ps1, the host SDK's rexglue.exe from tools\build_host.bat), then post_codegen.py in full.
if not exist "%ROOT%\app\generated\rexglue.cmake" (
  echo build_pc: no app\generated\rexglue.cmake: running the code generator and post_codegen first
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0codegen.ps1" || exit /b 1
  python "%~dp0post_codegen.py" || exit /b 1
)
if not exist "%BUILD_DIR%\CMakeCache.txt" (
  cmake -S "%ROOT%\app" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release %REX_CMAKE_COMPILERS% -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_USE_D3D12=ON -DREXGLUE_USE_VULKAN=ON "-DREXSDK_DIR=%REXSDK_DIR%" -DBANJO_RENUT_RENDERER=%RENUT% || exit /b 1
)
cmake --build "%BUILD_DIR%" -j 14 %*
