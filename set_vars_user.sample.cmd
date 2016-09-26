@rem Rename this file to "set_vars_user.cmd" to override default paths and options

@set cygwin32toolchain=%~d0\MSYS\cygwin
@set cygwin64toolchain=%~d0\MSYS\cygwin64
@set msys1toolchain=%~d0\MSYS\mingw32\msys\1.0
@set msys2x32toolchain=%~d0\MSYS\msys2-x32
@set msys2x64toolchain=%~d0\MSYS\msys2-x64

@set sign_code=NO
@set LANG=en_EN.UTF-8
@set verbose=NO
@set debug_log=NO

@set "dumpbin=C:\Program Files (x86)\Microsoft Visual Studio 12.0\VC\bin\amd64\dumpbin.exe"
@set "grep=%cygwin64toolchain%\bin\grep.exe"
