@echo off
rem Host build of the port's SDK (nfsmw-derived): codegen tool with share_registers etc.
call "%~dp0pc_env.bat" || exit /b 1
cmake -S "%~dp0..\sdk" -B "%~dp0..\out\host" -G Ninja -DCMAKE_BUILD_TYPE=Release %REX_CMAKE_COMPILERS% -DREXGLUE_ENABLE_TRACY=OFF
