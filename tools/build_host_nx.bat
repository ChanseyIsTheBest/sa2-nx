@echo off
call "%~dp0pc_env.bat" || exit /b 1
cmake --build "%~dp0..\out\host" --target %1 -j 14
