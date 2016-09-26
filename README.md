## Cygwin/MSYS connector
This helper tool for [ConEmu-Maximus5](https://conemu.github.io)
provides POSIX environment for:

* [Cygwin](https://www.cygwin.com/): `conemu-cyg-32.exe` and `conemu-cyg-64.exe`;
* [MSYS 1.0](http://www.mingw.org/wiki/msys): `conemu-msys-32.exe`;
* [MSYS 2.0](https://msys2.github.io/): `conemu-msys2-32.exe` and `conemu-msys2-64.exe`.


## How to use connector

How to install and use connector read in
[ConEmu docs](https://conemu.github.io/en/CygwinMsysConnector.html).

Please note, status of this plugin is ‘Experimental’.


## WARNING

* **Do not** run connector from cygwin or msys shell! Different cygwin/msys layers will cause problems!
* Connector might be started as [ROOT PROCESS](https://conemu.github.io/en/RootProcess.html)
  or from some native shell (like cmd.exe) already started in ConEmu.


## Screenshots
##### Just a `cat AnsiColors256.ans` from bash
![cygwin](https://github.com/Maximus5/cygwin-connector/wiki/cygwin-256colors.png)
##### 256 colors in Vim (Zenburn color scheme)
![cygwin](https://github.com/Maximus5/cygwin-connector/wiki/cygwin-vim-zenburn.png)


## License (BSD 3-clause)
    THIS SOFTWARE IS PROVIDED BY THE AUTHOR ''AS IS'' AND ANY EXPRESS OR
    IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
    OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.

## Some build notes

Some preparations may be required to build ‘connector’ from sources.

### Common

Rename `set_vars_user.sample.cmd` to `set_vars_user.cmd` if you need to define your own paths to used toolchains (cygwin, msys).

### MinGW / MSys 1.0

* Run `bin\mingw-get.exe`.
* Select ‘MSYS System Builder / msys-gcc’ to install and ‘Apply changes’ from menu.

### Cygwin 32/64 bit

I used to install 32bit and 64bit cygwin toolchains into separate folders to avoid path problems.

* In cygwin setup utility (e.g. `setup-x86_64.exe`) type ‘g++’ in the ‘search’ field.
* Install ‘gcc-g++: GNU Compiler Collection (C++)’.
