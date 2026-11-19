@echo off
rem Builds the Switch NRO: app\out\switch\renut.nro
rem Usage: tools\build_switch.bat [extra cmake --build args]
rem   NVK_SDK (env) overrides the Mesa/NVK SDK folder (must contain lib\libvulkan.a).
rem   BUILD_DIR (env) another build folder; CMAKE_EXTRA (env) more configure options for a new folder
rem   (docs/pgo-lto.md of banjo-nx: -DBANJO_PGO=generate, or -DBANJO_PGO=use -DBANJO_SWITCH_LTO=ON).
rem   GUARD_FORMS (env) ON / OFF: the localized copies' guard forms (SASRT_HOT_GUARD_FORMS; port 78, sasrt-nx's idea 27),
rem   set in the folder's cache when it holds the other value. Unset, a new folder has none (the option's Switch
rem   default): tools\build_switch_guard.bat builds app\out\switch-guard with them, for guard and verify runs.
rem   IN_PLACE (env) ON / OFF: the localized copies in place of their generated functions (SASRT_HOT_IN_PLACE; port
rem   81, sasrt-nx's machine copies in place: no hooks, no guard, no per-copy keys, the copies inlinable), set in the
rem   folder's cache when it holds the other value. Unset, a new folder has them in place unless it has the guard forms
rem   (the option's Switch default), and GUARD_FORMS=ON sets them OFF (a guard needs its hooks). Each form has its own
rem   PGO profile: pgo\ (hooked) and pgo-inplace\ (in place), docs/building.md.
rem Uses the VS-bundled CMake/Ninja (Windows paths) and devkitA64 GCC via the toolchain file.
set "VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
set "PATH=C:\Python314;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
set "DEVKITPRO=C:/devkitPro"
set "ROOT=%~dp0.."
if "%NVK_SDK%"=="" set "NVK_SDK=C:/devkitPro/mesa-switch-main-nfsmw-source/mesa-switch-main/mesa-unified-install/opt/devkitpro/portlibs/switch"
if "%BUILD_DIR%"=="" set "BUILD_DIR=%ROOT%\app\out\switch"
if "%JOBS%"=="" set "JOBS=10"
if not exist "%BUILD_DIR%\CMakeCache.txt" (
  cmake -S "%ROOT%\app" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
    "-DCMAKE_TOOLCHAIN_FILE=%ROOT%\tools\switch\cmake\switch-devkitA64.cmake" ^
    "-DREXSDK_DIR=%ROOT%\sdk" "-DREXGLUE_SWITCH_NVK_SDK=%NVK_SDK%" ^
    -DPYTHON_EXECUTABLE=C:/Python314/python.exe -DPython3_EXECUTABLE=C:/Python314/python.exe %CMAKE_EXTRA% || exit /b 1
)
rem renut-ultimate (2026-11-11, port 78; sasrt-nx's idea 27, its -GuardForms): GUARD_FORMS re-configures the folder only
rem when its cache holds the other value (the hot sources then compile again in the other form).
rem renut-ultimate (2026-11-11, port 81; sasrt-nx's machine copies in place, its -InPlace): IN_PLACE the same way, in
rem the same re-configure; GUARD_FORMS=ON with IN_PLACE unset asks for the hooked copies too.
set "BANJO_IN_PLACE=%IN_PLACE%"
if "%BANJO_IN_PLACE%"=="" if /i "%GUARD_FORMS%"=="ON" set "BANJO_IN_PLACE=OFF"
set "BANJO_RECONFIGURE="
if not "%BANJO_IN_PLACE%"=="" findstr /x /i /c:"SASRT_HOT_IN_PLACE:BOOL=%BANJO_IN_PLACE%" "%BUILD_DIR%\CMakeCache.txt" >nul || set "BANJO_RECONFIGURE=-DSASRT_HOT_IN_PLACE=%BANJO_IN_PLACE%"
if not "%GUARD_FORMS%"=="" findstr /x /i /c:"SASRT_HOT_GUARD_FORMS:BOOL=%GUARD_FORMS%" "%BUILD_DIR%\CMakeCache.txt" >nul || set "BANJO_RECONFIGURE=%BANJO_RECONFIGURE% -DSASRT_HOT_GUARD_FORMS=%GUARD_FORMS%"
if not "%BANJO_RECONFIGURE%"=="" (
  cmake %BANJO_RECONFIGURE% "%BUILD_DIR%" || exit /b 1
)
cmake --build "%BUILD_DIR%" -j %JOBS% %*
