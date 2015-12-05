## Cygwin/MSYS connector
This helper tool for [ConEmu-Maximus5](https://conemu.github.io)
provides POSIX environment for:

* [Cygwin](https://www.cygwin.com/): `conemu-cyg-32.exe` and `conemu-cyg-64.exe`;
* [MSYS 1.0](http://www.mingw.org/wiki/msys): `conemu-msys-32.exe`;
* [MSYS 2.0](https://msys2.github.io/): `conemu-msys2-32.exe` and `conemu-msys2-64.exe`.


## How to use connector

How to install and use connector read in
[ConEmu docs](CygwinMsysConnector.html).

Please note, status of this plugin is ‘Experimental’.


## WARNING

* **Do not** run connector from cygwin or msys shell! Different cygwin/msys layers will cause problems!
* Connector might be started as [ROOT PROCESS](https://conemu.github.io/en/RootProcess.html) or from some native shell (like cmd.exe) already started in ConEmu.


## Screenshots
##### Just a `cat AnsiColors256.ans` from bash
![cygwin](https://github.com/Maximus5/cygwin-connector/wiki/cygwin-256colors.png)
##### 256 colors in Vim (Zenburn color scheme)
![cygwin](https://github.com/Maximus5/cygwin-connector/wiki/cygwin-vim-zenburn.png)


## License (BSD 3-clause)
    THIS SOFTWARE IS PROVIDED BY THE AUTHOR ''AS IS'' AND ANY EXPRESS OR
    IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
    OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
