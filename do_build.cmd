@echo off

rem Build parameters
set sign_code=YES
set debug_log=YES

rem This defines paths to cygwin/msys toolchains
call "%~dp0set_vars.cmd"

rem User may call to build the target
rem For example: do_build cygwin32 conemu-cyg-32.exe
if NOT "%~1" == "" goto :%~1

rem Start build process for all toolchains
call "%~0" cygwin32 conemu-cyg-32.exe
call "%~0" cygwin64 conemu-cyg-64.exe
call "%~0" msys1 conemu-msys-32.exe
call "%~0" msys32 conemu-msys2-32.exe
call "%~0" msys64 conemu-msys2-64.exe

rem Final cleaning
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
set RCFLAGS=-F pe-i386 %LOGGING%
goto :EOF

:bit64
set DIRBIT=64
set RCFLAGS=-F pe-x86-64 %LOGGING%
goto :EOF



:build
setlocal
call cecho /yellow "Using: `%toolchain%` for `%~2` %DIRBIT%bit"

if exist ConEmuT.res.o ( del ConEmuT.res.o > nul )
echo Compiling resources
windres %RCFLAGS% -i ConEmuT.rc -o ConEmuT.res.o 2> "%~2.log"
if errorlevel 1 goto print_errors

echo Compiling code and linking
g++ %LOGGING% ConEmuT.cpp -o %2 -Xlinker ConEmuT.res.o -mconsole -m%DIRBIT% %NO_DEBUG% 2> "%~2.log"
if errorlevel 1 goto print_errors

if NOT "%sign_code%" == "YES" goto skip_sign
echo Signing `%~2`
call sign "%~2" > nul
:skip_sign

endlocal
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
