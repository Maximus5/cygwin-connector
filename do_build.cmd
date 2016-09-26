@echo off

setlocal

rem Build parameters
set sign_code=YES
set debug_log=NO

set NO_DEBUG=-s -O3

rem This defines paths to cygwin/msys toolchains
call "%~dp0set_vars.cmd"

rem User may call to build the target
rem For example: do_build cygwin32 conemu-cyg-32.exe
if NOT "%~1" == "" goto :%~1

rem Start build process for all toolchains
rem *** conemu-cyg-32.exe
call "%~0" cygwin32
rem *** conemu-cyg-64.exe
call "%~0" cygwin64
rem *** conemu-msys-32.exe
call "%~0" msys1
rem *** conemu-msys2-32.exe
call "%~0" msys32
rem *** conemu-msys2-64.exe
call "%~0" msys64

rem Final cleaning
if exist ConEmuT.res.o ( del ConEmuT.res.o > nul )

goto :EOF

:cygwin32
set toolchain=%cygwin32toolchain%
set PATH=%cygwin32toolchain%\bin;%PATH%
set exe_name=%cygwin32exe%
call :bit32
goto :build

:cygwin64
set toolchain=%cygwin64toolchain%
set PATH=%cygwin64toolchain%\bin;%PATH%
set exe_name=%cygwin64exe%
call :bit64
goto :build

:msys1
set toolchain=%msys1toolchain%
set PATH=%msys1toolchain%\bin;%msys1toolchain%\..\..\mingw\bin;%PATH%
set exe_name=%msys1exe%
set USE_GCC_STATIC=
call :bit32
goto :build

:msys32
set toolchain=%msys2x32toolchain%
set PATH=%msys2x32toolchain%\usr\bin;%PATH%
set exe_name=%msys2x32exe%
call :bit32
goto :build

:msys64
set toolchain=%msys2x64toolchain%
set PATH=%msys2x64toolchain%\usr\bin;%PATH%
set exe_name=%msys2x64exe%
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
call cecho /yellow "Using: `%toolchain%` for `%exe_name%` %DIRBIT%bit"

if exist ConEmuT.res.o ( del ConEmuT.res.o > nul )
echo Compiling resources
windres %RCFLAGS% -i ConEmuT.rc -o ConEmuT.res.o 2> "%exe_name%.log"
if errorlevel 1 goto print_errors

echo Compiling code and linking
gcc -fno-rtti %LOGGING% ConEmuT.cpp -o %exe_name% %USE_GCC_STATIC% -Xlinker ConEmuT.res.o -mconsole -m%DIRBIT% %NO_DEBUG% 2> "%exe_name%.log"
if errorlevel 1 goto print_errors

if NOT "%sign_code%" == "YES" goto skip_sign
echo Signing `%exe_name%`
call sign "%exe_name%" > nul
:skip_sign

endlocal

rem if not defined dumpbin goto skip_imp
rem if not exist "%dumpbin%" goto skip_imp
rem if not defined grep goto skip_imp
rem if not exist "%grep%" goto skip_imp
"%dumpbin%" /IMPORTS %exe_name% | "%grep%" -G ".*\.dll"
:skip_imp

call cecho /green "Build succeeded: %exe_name%"
goto :EOF

:print_errors
set "err_msg=Build failed: %errorlevel%"
echo --- build errors (begin)
type "%exe_name%.log"
echo --- build errors (end)
call cecho "%err_msg%"
exit /B 99
endlocal
