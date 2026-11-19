@echo off
call "%~dp0pc_env.bat" || exit /b 1
cmake -S "D:/Banjo kazooie port/rexglue-sdk" -B "D:/Banjo kazooie port/out/host" -G Ninja -DCMAKE_BUILD_TYPE=Release %REX_CMAKE_COMPILERS% -DREXGLUE_ENABLE_TRACY=OFF
