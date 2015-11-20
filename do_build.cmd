@echo off

set cygwin32toolchain=T:\cygwin
set cygwin64toolchain=T:\cygwin64
set msys1toolchain=T:\MSYS\mingw\msys\1.0
set msys2x32toolchain=T:\MSYS\msys2-x32
set msys2x64toolchain=T:\MSYS\msys2-x64

if NOT "%~1" == "" goto :%~1

rem msys1 buld is failed yet
rem call "%~0" msys1 conemu-msys-32.exe

call "%~0" cygwin32 conemu-cyg-32.exe
call "%~0" cygwin64 conemu-cyg-64.exe
call "%~0" msys32 conemu-msys2-32.exe
call "%~0" msys64 conemu-msys2-64.exe

if exist ConEmuT.res.o ( del ConEmuT.res.o > nul )

goto :EOF

:cygwin32
set toolchain=%cygwin32toolchain%
set PATH=%cygwin32toolchain%\bin;%PATH%
call :bit32
goto :build

:cygwin64
set toolchain=%cygwin64toolchain%
set PATH=%cygwin64toolchain%\bin;%PATH%
call :bit64
goto :build

:msys1
set toolchain=%msys1toolchain%
set PATH=%msys1toolchain%\bin;%PATH%
call :bit32
goto :build

:msys32
set toolchain=%msys2x32toolchain%
set PATH=%msys2x32toolchain%\usr\bin;%PATH%
call :bit32
goto :build

:msys64
set toolchain=%msys2x64toolchain%
set PATH=%msys2x64toolchain%\usr\bin;%PATH%
call :bit64
goto :build

:bit32
set DIRBIT=32
set RCFLAGS=-F pe-i386
goto :EOF

:bit64
set DIRBIT=64
set RCFLAGS=-F pe-x86-64
goto :EOF

cd /d "%~dp0"

if NOT "%~1" == "" goto :do_build

set CHERE_INVOKING=1

rem OK begin
call "%~0" "%~d0\cygwin\bin" "" 32 "i686-pc-cygwin-" "i686-pc-mingw32-"
call "%~0" "%~d0\cygwin\bin" "conemu-cyg-64.exe" 64 "x86_64-pc-cygwin-" "x86_64-w64-mingw32-"
call "%~0" "%~d0\GitSDK\usr\bin" "conemu-msys2-64.exe" 64
rem OK ends


rem call "%~0" "%~d0\cygwin64\bin" "conemu-cyg-64.exe"
rem call "%~0" "%~d0\GitSDK\mingw32\bin;%~d0\GitSDK\usr\bin" "conemu-msys2-32.exe"
rem call "%~0" "%~d0\MinGW\bin;%~d0\MinGW\msys32\bin" "conemu-msys1-32.exe"


goto :EOF

:build
rem setlocal
call cecho /yellow "Using: `%toolchain%` for `%~2` %DIRBIT%bit"
rem set "PATH=%~1;%windir%;%windir%\System32;%ConEmuDir%;%ConEmuBaseDir%;"

if exist ConEmuT.res.o ( del ConEmuT.res.o > nul )
windres %RCFLAGS% -i ConEmuT.rc -o ConEmuT.res.o 2> "%~2.log"
if errorlevel 1 goto print_errors

g++ ConEmuT.cpp -o %2 -Xlinker ConEmuT.res.o -mconsole -m%DIRBIT% 2> "%~2.log"
if errorlevel 1 goto print_errors

rem endlocal
rem call sign %2
call cecho /green "Build succeeded: %2"
goto :EOF

:print_errors
set "err_msg=Build failed: %errorlevel%"
echo --- build errors (begin)
type "%2.log"
echo --- build errors (end)
call cecho "%err_msg%"
rem exit /B 99
endlocal
