@echo off

if NOT "%~1" == "" goto :do_build

set CHERE_INVOKING=1

call "%~0" "%~d0\cygwin\bin" "conemu-cyg-32.exe"

rem call "%~0" "%~d0\cygwin64\bin" "conemu-cyg-64.exe"

rem call "%~0" "%~d0\GitSDK\mingw32\bin;%~d0\GitSDK\usr\bin" "conemu-msys2-32.exe"
rem call "%~0" "%~d0\GitSDK\mingw64\bin;%~d0\GitSDK\usr\bin" "conemu-msys2-64.exe"

rem call "%~0" "%~d0\MinGW\bin;%~d0\MinGW\msys32\bin" "conemu-msys1-32.exe"


goto :EOF

:do_build
setlocal
call cecho /yellow "Using: %~1"
set "PATH=%~1;%windir%;%windir%\System32;%ConEmuDir%;%ConEmuBaseDir%;"
gcc ConEmuT.cpp -o %2 2> "%2.log"
if errorlevel 1 (
  call cecho "Build failed"
  rem exit /B 99
) else (
  call cecho /green "Build succeeded"
)
endlocal
