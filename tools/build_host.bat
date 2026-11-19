@echo off
call "%~dp0pc_env.bat" || exit /b 1
cmake --build "D:/Banjo kazooie port/out/host" --target %1 -j 14
