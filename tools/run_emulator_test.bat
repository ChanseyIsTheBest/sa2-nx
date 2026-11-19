@echo off
rem banjo-nx: starts the Switch emulator (Ryujinx / Ryubing) on the test NRO in its SD card folder.
rem The NRO's renut.toml there drives the menus (input_script), writes a capture every 15 s and its logs into
rem %APPDATA%\Ryujinx\sdcard\switch\banjo-nx\ ; Claude records the window and reads the results afterwards.
rem Double-click it yourself: Claude does not start the emulator (an unofficial build).
set "NRO=%APPDATA%\Ryujinx\sdcard\switch\banjo-nx\banjo-nx.nro"
set "EXE=D:\New folder\ryujinx-canary-1.3.351\publish\Ryujinx.exe"
if not exist "%EXE%" set "EXE=D:\New folder\ryujinx-canary-1.3.351\Ryujinx.exe"
if not exist "%EXE%" (
  echo Ryujinx.exe not found. Extract ryujinx-canary-1.3.351-windows_x64.zip to "D:\New folder\ryujinx-canary-1.3.351".
  pause
  exit /b 1
)
if not exist "%NRO%" (
  echo The test NRO is missing: %NRO%
  pause
  exit /b 1
)
start "" "%EXE%" "%NRO%"
