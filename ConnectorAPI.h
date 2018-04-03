
/*
Copyright (c) 2015-present Maximus5
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. The name of the authors may not be used to endorse or promote products
   derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR ''AS IS'' AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#pragma once

enum RequestTermConnectorMode
{
	rtc_Start = 1,
	rtc_Stop  = 2,
};

#ifndef WRITE_PROCESSED_STREAM_DEFINED
/* excerpt from Ansi.h begin */
enum WriteProcessedStream
{
	// ...
	wps_Output = 1,
	wps_Error  = 2,
	// ...
};
/* excerpt from Ansi.h end */
#endif

// this is bit-mask
enum ReadInputResult
{
	rir_None  = 0, // there were no INPUT_RECORD events
	rir_Ready = 1, // buffer contains result_count events 
	rir_More  = 2, // console queue has more events
	rir_Ready_More = rir_Ready|rir_More,
};

struct RequestTermConnectorParm
{
	// [IN]  size in bytes of this structure
	DWORD cbSize;
	// [IN]  requrested operation
	enum RequestTermConnectorMode Mode;

	// [IN]  dump initialization steps to console
	BOOL bVerbose;

	// [IN]  ttyname(STDOUT_FILENO)
	LPCSTR pszTtyName;
	// [IN]  $TERM
	LPCSTR pszTerm;

	// [IN]  mount prefix (e.g. /cygdrive or /mnt)
	LPCSTR pszMntPrefix;

	// [OUT] If there were any errors, here may be some details
	LPCSTR pszError;

	// [OUT] This one is UNICODE
	enum ReadInputResult (WINAPI* ReadInput)(PINPUT_RECORD buffer, DWORD buffer_count, PDWORD result_count);
	// [OUT] But this is ANSI (UTF-8 is expected)
	//       cbWrite==-1 : pBuffer contains ASCIIZ string, call strlen on it
	BOOL (WINAPI* WriteText)(LPCSTR pBuffer, DWORD cbWrite, PDWORD pcbWritten, enum WriteProcessedStream nStream);
};
