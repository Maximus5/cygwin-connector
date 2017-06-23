
/*
from https://github.com/Alexpux/MSYS2-packages/issues/265

Copyright (c) 2015-2017 Maximus5
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

#define _GNU_SOURCE
#ifdef __CYGWIN__
#include <cygwin/version.h>
#endif

#undef _USE_DEBUG_LOG_INPUT

//#define SHOW_CHILD_ERR_MSG
#undef SHOW_CHILD_ERR_MSG

#if (__GNUC_MINOR__ >= 9) || (CYGWIN_VERSION_API_MINOR>=93)
#define HAS_FORKPTY
#pragma message "Has forkpty"
#else
#undef HAS_FORKPTY
#pragma message "Does NOT have forkpty"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <process.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/termios.h>

#include <w32api/wtypes.h>
#include <w32api/wincon.h>
#include <w32api/winuser.h>

#include <unistd.h>
#include <utmp.h>

// exists in cygwin+msys2
#if defined(HAS_FORKPTY)
#include <pty.h>
#endif

#define _max(a,b) (((a) > (b)) ? (a) : (b))

bool verbose = false;
bool debugger = false;
static int gnLogFile = -1;

static void write_verbose(const char *buf, ...);
static void print_version();

#include "version.h"


enum RequestTermConnectorMode
{
	rtc_Start = 1,
	rtc_Stop  = 2,
};

enum WriteProcessedStream
{
	wps_Output = 1,
	wps_Error  = 2,
};

typedef struct tag_RequestTermConnectorParm
{
	// [IN]  size in bytes of this structure
	DWORD cbSize;
	// [IN]  requrested operation
	RequestTermConnectorMode Mode;

	// [IN]  dump initialization steps to console
	BOOL bVerbose;

	// [IN]  ttyname(STDOUT_FILENO)
	LPCSTR pszTtyName;
	// [IN]  $TERM
	LPCSTR pszTerm;

	// [OUT] If there were any errors, here may be some details
	LPCSTR pszError;

	// [OUT] This one is UNICODE
	BOOL (WINAPI* ReadInput)(PINPUT_RECORD,DWORD,PDWORD);
	// [OUT] But this is ANSI (UTF-8 is expected)
	//       cbWrite==-1 : pBuffer contains ASCIIZ string, call strlen on it
	BOOL (WINAPI* WriteText)(LPCSTR pBuffer, DWORD cbWrite, PDWORD pcbWritten, WriteProcessedStream nStream);
} RequestTermConnectorParm;

static HMODULE hConEmuHk = NULL;
static RequestTermConnectorParm Connector = {};

typedef int (WINAPI* RequestTermConnector_t)(/*[IN/OUT]*/RequestTermConnectorParm* Parm);
static RequestTermConnector_t fnRequestTermConnector = NULL;

static int RequestTermConnector()
{
	int iRc;
	char sModule[] =
		#if defined(__x86_64__)
			"ConEmuHk64.dll"
		#else
			"ConEmuHk.dll"
		#endif
			;
	const char* basedir;

	basedir = getenv("ConEmuBaseDir");
	if (basedir && *basedir)
	{
		char* path;
		path = (char*)malloc(strlen(basedir)+2+strlen(sModule));
		if (path)
		{
			strcpy(path, basedir);
			strcat(path, "\\");
			strcat(path, sModule);
			hConEmuHk = LoadLibraryA(path);
			free(path);
		}
	}
	if (hConEmuHk == NULL)
	{
		hConEmuHk = LoadLibraryA(sModule);
	}
	if (hConEmuHk == NULL)
	{
		write_verbose("\r\n{PID:%u} %s is not found, exiting\r\n", getpid(), sModule);
		return -1;
	}

	fnRequestTermConnector = (RequestTermConnector_t)GetProcAddress(hConEmuHk, "RequestTermConnector");
	if (fnRequestTermConnector == NULL)
	{
		write_verbose("\r\n{PID:%u} RequestTermConnector function is not found, exiting\r\n", getpid());
		iRc = -1;
	}
	else
	{

		// Prepare arguments
		memset(&Connector, 0, sizeof(Connector));
		Connector.cbSize = sizeof(Connector);
		Connector.Mode = rtc_Start;
		Connector.pszTtyName = ttyname(STDOUT_FILENO);
		Connector.pszTerm = getenv("TERM");

		iRc = fnRequestTermConnector(&Connector);

		if (iRc != 0)
		{
			write_verbose("\r\n{PID:%u} RequestTermConnector failed (%i). %s\r\n", getpid(), iRc, Connector.pszError ? Connector.pszError : "");
			iRc = -1;
		}
		else if (!Connector.ReadInput || !Connector.WriteText)
		{
			write_verbose("\r\n{PID:%u} RequestTermConnector returned NULL. %s\r\n", getpid(), Connector.pszError ? Connector.pszError : "");
			iRc = -1;
		}
	}

	if (iRc != 0)
	{
		FreeLibrary(hConEmuHk);
		hConEmuHk = NULL;
	}
	return iRc;
}

static void StopTermConnector()
{
	if (fnRequestTermConnector)
	{
		Connector.cbSize = sizeof(Connector);
		Connector.Mode = rtc_Stop;
		fnRequestTermConnector(&Connector);
	}

	memset(&Connector, 0, sizeof(Connector));

	if (gnLogFile >= 0)
	{
		if (verbose)
			write_verbose("\r\n\033[31;40m{PID:%u} closing log file (%i)\033[m\r\n", getpid(), gnLogFile);
		close(gnLogFile);
		gnLogFile = -1;
	}

	if (hConEmuHk)
	{
		FreeLibrary(hConEmuHk);
	}
}

#if defined(_USE_DEBUG_LOG)
#define DEBUG_LOG_MAX_BUFFER 1024
static void debug_log(const char* text)
{
	OutputDebugStringA(text);
}
static void debug_log_format(const char* format,...)
{
	va_list ap;
	char buf[DEBUG_LOG_MAX_BUFFER];
	va_start(ap, format);
	vsnprintf(buf, sizeof buf, format, ap);
	va_end(ap);
	debug_log(buf);
}
#else
#define debug_log(text)
#define debug_log_format(format...)
#endif



static int pty_fd = -1, pty_err = -1;
static int slave_std_err = -1, slave_std_out = -1;
static pid_t pid = -1;
static HANDLE input_thread = NULL;
static DWORD input_tid = 0;
static void stop_threads();
static bool termination = false;
static int check_child(bool force_print = false);

static BOOL WINAPI CtrlHandlerRoutine(DWORD dwCtrlType)
{
	// We do not expect to receive CTRL_C_EVENT/CTRL_BREAK_EVENT because of ProtectCtrlBreakTrap

	if (verbose)
	{
		write_verbose("\r\n\033[31;40m{PID:%u} CtrlHandlerRoutine(%u) triggered\033[m\r\n", getpid(), dwCtrlType);
	}

	switch (dwCtrlType)
	{
	case CTRL_C_EVENT:
		break;
	case CTRL_BREAK_EVENT:
		return TRUE; // bypass
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		if (pid > 0)
			kill(-pid, SIGHUP);
		break;
	default:
		/*sprintf(szType, "ID=%u", dwCtrlType)*/;
	}

	return FALSE;
}

static void stop_waiting_debugger(int sig)
{
	debugger = false;
}

static void sigexit(int sig)
{
	if (verbose)
	{
		write_verbose("\r\n\033[31;40m{PID:%u} signal %i received\033[m\r\n", getpid(), sig);
	}
	else
	{
		debug_log_format("signal %i received, pid=%i\n", sig, pid);
	}

	switch (sig)
	{
	case SIGINT:
		// We do not expect to receive SIGINT because of ProtectCtrlBreakTrap
		if (verbose)
			write_verbose("\r\n\033[31;40m{PID:%u} Passing ^C to client\033[m\r\n", getpid());
		write(pty_fd, "\3", 1);
		//if (pid > 0)
		//	kill(pid, sig); // or kill(-group, sig)
		return;
	}

	if (pid > 0)
		kill(-pid, SIGHUP);
	stop_threads();
	signal(sig, SIG_DFL);
	kill(getpid(), sig);
}

static bool gb_sigusr1 = false;
static void sigusr1(int sig)
{
	if (sig == SIGUSR1)
	{
		if (verbose)
			write_verbose("\033[%u;40m{PID:%u} SIGUSR1 received\033[m\r\n", pid?31:33, getpid());

		gb_sigusr1 = true;

		signal(SIGUSR1, SIG_DFL);
	}
}

void sigusr1_throw(pid_t a_pid)
{
	if (verbose)
		write_verbose("\033[%u;40m{PID:%u} raising SIGUSR1 in pid=%i\033[m\r\n", pid?31:33, getpid(), a_pid);
	kill(a_pid, SIGUSR1);
}

char * const * child_argv = NULL;
const char * work_dir = NULL;

static void print_shell_args()
{
	char* cwd = work_dir ? NULL : getcwd(NULL, 0);
	write_verbose("\033[33;40m{PID:%u} shell: `%s`", getpid(), child_argv[0]);
	for (int c = 1; child_argv[c]; c++)
		write_verbose(" `%s`", child_argv[c]);
	write_verbose("\033[m\r\n");
	write_verbose("\033[33;40m{PID:%u}   dir: `%s`\033[m\r\n", getpid(), work_dir ? work_dir : cwd ? cwd : "<%cd%>");
	free(cwd);
}


static void sigfault(int sig)
{
	if (sig == SIGSEGV)
	{
		write_verbose("\033[%u;40m{PID:%u} Failed to run shell (SIGSEGV)\033[m\r\n", pid?31:33, getpid());
		if (!verbose)
			print_shell_args();
		// if we exit immediately, some versions of cygwin/msys will not be able to print our message
		sleep(1);
		exit(EFAULT);
	}
}


static bool write_console(const char *buf, int len, WriteProcessedStream strm = wps_Output)
{
	if (len == -1)
		len = strlen(buf);

	debug_log_format("%u:PID=%u:TID=%u: writing ANSI: %s\n", GetTickCount(), getpid(), GetCurrentThreadId(), (len > (DEBUG_LOG_MAX_BUFFER-80)) ? "<Too long text to use debug_log_format>" : buf);

	while (len > 0)
	{
		DWORD written = 0; BOOL bRc;
		if (Connector.WriteText)
		{
			// Server side, initialized

			// First, log the string if required
			if (gnLogFile >= 0)
			{
				write(gnLogFile, buf, len);
			}

			// Dump to console
			bRc = Connector.WriteText(buf, len, &written, wps_Output);
		}
		else if (pid != 0) // Not-a-child or before-fork
		{
			// Server side, before initialization
			// We need to call API directly, because fwrite/printf/...
			// may break colors, if they were written in wrong moment
			bRc = WriteConsoleA(GetStdHandle((strm == wps_Output) ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE), buf, len, &written, NULL);
		}
		else
		{
			// Child (client) side
			ssize_t term_written;
			int h_out = (strm == wps_Output) ? slave_std_out : slave_std_err;
			//if (h_out < 0) h_out = STDOUT_FILENO;
			term_written = write(h_out, buf, len);
			bRc = (term_written > 0);
			written = term_written;
		}

		if (!bRc)
			return false;

		len -= written;
		buf += written;
	}

	return true;
}

// Don't check for `verbose` flag here, the function may be used in other places
static void write_verbose(const char *buf, ...)
{
	//OutputDebugStringA(buf); -- no need, Debug versions of ConEmuHk dump ANSI output automatically
	char szBuf[1024]; // don't use static here!
	va_list args;
	int ilen = -1;
	if (strchr(buf, '%'))
	{
		va_start(args, buf);
		ilen = vsnprintf(szBuf, sizeof(szBuf) - 1, buf, args);
		va_end(args);
	}
	write_console((ilen > 0) ? szBuf : buf, -1, wps_Error);
}

#if defined(SHOW_CHILD_ERR_MSG)
void child_msg_box(const char* text, const char* title)
{
	MessageBox(NULL, text, title, MB_SYSTEMMODAL);
}
#else
#define child_msg_box(text,title)
#endif

void child_err_msg(const char* reason)
{
	int e = errno;
	const char* pszErDescr = strerror(errno);
	#if defined(SHOW_CHILD_ERR_MSG)
	char* pchMsg = (char*)malloc(255+pszErDescr?strlen(pszErDescr):0);
	if (pchMsg)
	{
		sprintf(pchMsg, "{PID:%u} %s (%i): %s", getpid(), reason ? reason : "<unknown fail>", e, pszErDescr);
		child_msg_box(pchMsg, "connector");
		free(pchMsg);
	}
	#endif
	write_verbose("\033[30;41m\033[K{PID:%u} %s (%i): %s\033[m\r\n", getpid(), reason ? reason : "<unknown fail>", e, pszErDescr);
}

static int resize_pty(int pty, struct winsize *winp)
{
	int iRc = -99;

	if (pty >= 0)
	{
		// SIGWINCH signal is sent to the foreground process group
		iRc = ioctl(pty, TIOCSWINSZ, winp);

		debug_log_format("resize_pty: TIOCSWINSZ(pty=%i,cell={%i,%i},pix={%i,%i})=%i\n", pty, winp->ws_col, winp->ws_row, winp->ws_xpixel, winp->ws_ypixel, iRc);

		if (verbose)
		{
			if (iRc == -1)
				write_verbose("\033[31;40m{PID:%u} ioctl(%i,TIOCSWINSZ,(%i,%i)) failed (%i): %s\033[m\r\n", getpid(), pty, winp->ws_col, winp->ws_row, errno, strerror(errno));
			else
				write_verbose("\033[31;40m{PID:%u} ioctl(%i,TIOCSWINSZ,(%i,%i)) succeeded (%i)\033[m\r\n", getpid(), pty, winp->ws_col, winp->ws_row, iRc);
		}

		if (gnLogFile >= 0)
		{
			char szLogSize[80];
			sprintf(szLogSize, "\x1B]9;11;\"TIOCSWINSZ(%i,%i) %s\"\x07", winp->ws_col, winp->ws_row, (iRc == -1) ? "failed" : "succeeded");
			write(gnLogFile, szLogSize, strlen(szLogSize));
		}
	}
	else
	{
		debug_log_format("resize_pty: invalid pty\n");
	}

	return iRc;
}

static bool query_console_size(struct winsize* winp)
{
	bool bRc = false;
	memset(winp, 0, sizeof(winp));
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};
	if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
	{
		winp->ws_row = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
		winp->ws_col = csbi.dwSize.X;
		bRc = true;
	}
	else
	{
		winp->ws_row = 25;
		winp->ws_col = 80;
	}
	winp->ws_xpixel = winp->ws_col * 3;
	winp->ws_ypixel = winp->ws_row * 5;
	return bRc;
}

static DWORD WINAPI read_input_thread( void * )
{
	while (!termination)
	{
		INPUT_RECORD r = {}; DWORD nReady = 0;
		if (Connector.ReadInput(&r, 1, &nReady) && nReady)
		{
			#if defined(_USE_DEBUG_LOG_INPUT)
			debug_log_format("read_input_thread: event %u received\n", r.EventType);
			#endif
			switch (r.EventType)
			{
			case WINDOW_BUFFER_SIZE_EVENT:
				{
					winsize winp;
					debug_log_format("read_input_thread: WindowBufferSize (%i,%i)\n", r.Event.WindowBufferSizeEvent.dwSize.X, r.Event.WindowBufferSizeEvent.dwSize.Y);
					if (query_console_size(&winp))
					{
						if (pty_fd >= 0)
							resize_pty(pty_fd, &winp);
						else
							debug_log_format("read_input_thread: invalid pty_fd\n");
						if (pty_err >= 0)
							resize_pty(pty_err, &winp);
					}
					else
					{
						debug_log_format("read_input_thread: query_console_size failed!!!\n");
					}
				}
				break;
			case KEY_EVENT:
				if (r.Event.KeyEvent.uChar.UnicodeChar
					&& r.Event.KeyEvent.bKeyDown)
				{
					char s[5];
					int len = WideCharToMultiByte(CP_UTF8, 0, &r.Event.KeyEvent.uChar.UnicodeChar, 1, s, sizeof(s)-1, 0, 0);
					if (len > 0)
					{
						s[len] = 0;
						ssize_t written = write(pty_fd, s, len);
						#if defined(_USE_DEBUG_LOG_INPUT)
						debug_log_format("read_input_thread: `%s` written %i of %i bytes\n", s, written, len);
						#endif
					}
				}
				break; // KEY_EVENT
			}
		}
	}
	return 0;
}

static void stop_threads()
{
	termination = true;

	if (verbose)
	{
		write_verbose("\r\n\033[31;40m{PID:%u} Stopping our threads\033[m\r\n", getpid());
	}

	StopTermConnector();

	if (input_thread && (WaitForSingleObject(input_thread, 5000) == WAIT_TIMEOUT))
	{
		TerminateThread(input_thread, 100);
	}
}

static int process_pty(int& pty, char* buf, const int bufCount, const int preferredCount)
{
	debug_log_format("%u:PID=%u:TID=%u: calling read(%i)\n", GetTickCount(), getpid(), GetCurrentThreadId(), pty);
	int len = read(pty, buf, bufCount);

	if (len > 0)
	{
		while ((len+4) < preferredCount)
		{
			int addLen = read(pty, buf+len, bufCount-len);
			if (addLen <= 0)
				break;
			len += addLen;
		}
		buf[len] = 0;
		write_console(buf, len, (pty == pty_err) ? wps_Error : wps_Output);
	}
	else
	{
		if (verbose)
		{
			write_verbose("\r\n\033[31;40m{PID:%u} read(pty=%i) failed (len=%i,errno=%i): %s\033[m\r\n", getpid(), pty, len, errno, strerror(errno));
			check_child();
		}
		pty = -1;
	}

	return len;
}

static int check_child(bool force_print /*= false*/)
{
	if (pid <= 0)
		return -1;

	int status = 0, wait_rc;

	debug_log_format("%u:PID=%u:TID=%u: calling waitpid(%i)\n", GetTickCount(), getpid(), GetCurrentThreadId(), pid);
	wait_rc = waitpid(pid, &status, WNOHANG);
	debug_log_format("%u:PID=%u:TID=%u: waitpid(%i) done rc=%u status=0x%X\n", GetTickCount(), getpid(), GetCurrentThreadId(), pid, wait_rc, status);

	if (wait_rc == pid)
	{
		if (verbose || force_print)
		{
			if (WIFEXITED(status))
				write_verbose("\r\n\033[31;40m{PID:%u} pid=%i was terminated, exitcode=%u", getpid(), pid, WEXITSTATUS(status), strerror(WEXITSTATUS(status)));
			else if (WIFSIGNALED(status))
				write_verbose("\r\n\033[31;40m{PID:%u} pid=%i was terminated by signal (%u): %s", getpid(), pid, WTERMSIG(status), strsignal(WTERMSIG(status)));
			else
				write_verbose("\r\n\033[31;40m{PID:%u} pid=%i was terminated, status=%i\033[m\r\n", getpid(), pid, status);
		}

		pid = -2;
	}
	else if (wait_rc == 0)
	{
		// One or more child(ren) exist
		if (force_print)
		{
			write_verbose("\r\n\033[31;40m{PID:%u} one or more children with pid=%i are alive\033[m\r\n", getpid(), pid);
		}
	}
	else if (verbose || force_print)
	{
		write_verbose("\r\n\033[31;40m{PID:%u} waitpid(%i) failed (%i): %s", getpid(), pid, errno, strerror(errno));
	}

	return (pid <= 0) ? -1 : 0;
}

static int run()
{
	fd_set fds;
	const int preferredCount = 280;
	const int bufCount = 4096;
	char buf[bufCount+1];
	struct timeval timeout = {0, 100000}, *timeout_p = 0;

	input_thread = CreateThread(NULL, 0, read_input_thread, NULL, 0, &input_tid);

	if (!input_thread || (input_thread == INVALID_HANDLE_VALUE))
	{
		return GetLastError() ? GetLastError() : 100;
	}

	for (;;)
	{
		FD_ZERO(&fds);
		if (pty_fd >= 0)
		{
			FD_SET(pty_fd, &fds);
			if (pty_err >= 0)
				FD_SET(pty_err, &fds);
		}
		else if (pid > 0)
		{
			if (check_child() == -1)
			{
				if (pty_fd < 0)
					break;
			}
			else
			{
				// Pty gone, but process still there: keep checking?
				timeout_p = &timeout;
			}
		}

		const int fdsmax = _max(pty_fd,pty_err) + 1;
		debug_log_format("%u:PID=%u:TID=%u: calling select on (%i,%i)\n", GetTickCount(), getpid(), GetCurrentThreadId(), pty_fd, pty_err);
		timeout.tv_usec = 100000;
		if (select(fdsmax, &fds, 0, 0, timeout_p) > 0)
		{
			if (pty_fd >= 0 && FD_ISSET(pty_fd, &fds))
			{
				process_pty(pty_fd, buf, bufCount, preferredCount);
				if (verbose && (pty_fd < 0))
					write_verbose("\r\n\033[31;40m{PID:%u} pty_fd set to -1\033[m\r\n", getpid(), pid);
			}

			if (pty_err >= 0 && FD_ISSET(pty_err, &fds))
			{
				process_pty(pty_err, buf, bufCount, preferredCount);
				if (verbose && (pty_err < 0))
					write_verbose("\r\n\033[31;40m{PID:%u} pty_err set to -1\033[m\r\n", getpid(), pid);
			}
		}
		else
		{
			debug_log_format("%u:PID=%u:TID=%u: select failed\n", GetTickCount(), getpid(), GetCurrentThreadId());
		}
	}

	check_child(true);

	stop_threads();

	return 0;
}

// switch `--keys` useful to check keyboard translations
static int test_read_keys()
{
	struct termios old = {0}, raw = {};

	print_version();
	printf("Starting raw conin reader, press Ctrl+C to stop\n");

	if (tcgetattr(0, &old) < 0)
		perror("tcgetattr()");
	raw = old;
	raw.c_lflag &= ~(ICANON|ECHO);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(0, TCSANOW, &raw) < 0)
		perror("tcsetattr()");

	for (;;)
	{
		int c = fgetc(stdin);
		if (c > 32 && c != 0x7F)
			printf("<x%02X:%c>", c, c);
		else if (c == 0xA)
			printf("<ENTER>\n");
		else
			printf("<x%02X>", c);
	}

	if (tcsetattr(0, TCSADRAIN, &old) < 0)
		perror ("reverting tcsetattr()");

	return 0;
}

static void print_version()
{
	printf("ConEmu cygwin/msys connector version %s\n", VERSION_S);
}

static void print_environ(bool bChild)
{
	char** pp = environ;

	if (!pp)
	{
		write_verbose("\033[31;40m{PID:%u} `environ` variable is NULL!\033[m\n", getpid());
		return;
	}

	write_verbose("\033[31;40m{PID:%u} printing `environ` lines\033[m\n", getpid());

	while (*pp)
	{
		write_console(*(pp++), -1);
		write_console("\n", 1);
	}

	write_verbose("\033[31;40m{PID:%u} end of `environ`, total=%i\033[m\n", getpid(), (pp - environ));
}

static int print_isatty(bool bChild)
{
	bool isTty = true;
	int iTty, errNoTty, errNoPgrp;
	pid_t ttyPgrp = -1;
	char* ttyName;

	for (int f = STDIN_FILENO; f <= STDERR_FILENO; f++)
	{
		ttyName = ttyname(f);

		errno = 0;
		iTty = isatty(f);
		errNoTty = errno;

		errno = 0;
		ttyPgrp = tcgetpgrp(f);
		errNoPgrp = errno;

		if (iTty == 1)
		{
			write_verbose("\033[%u;40m{PID:%u} %i: isatty()=%i; pgrp=%i; ttyname()=`%s`\033[m\n", pid?31:33, getpid(), f, iTty, ttyPgrp, ttyName?ttyName:"<NULL>");
		}
		else
		{
			write_verbose("\033[%u;40m{PID:%u} %i: isatty()=%i; pgrp=%i; ttyname()=`%s`\033[m\n", pid?31:33, getpid(), f, iTty, ttyPgrp, ttyName?ttyName:"<NULL>");
			isTty = false;
		}

		if (errNoTty)
			write_verbose("\033[%u;40m{PID:%u}    isatty error (%i): %s\033[m\n", pid?31:33, getpid(), errNoTty, strerror(errNoTty));
		if (errNoPgrp)
			write_verbose("\033[%u;40m{PID:%u}    tcgetpgrp error (%i): %s\033[m\n", pid?31:33, getpid(), errNoPgrp, strerror(errNoPgrp));
	}

	return isTty ? 0 : 1;
}


#if !defined(HAS_FORKPTY)
static int ce_createpty(const char* adescr, int *pmaster, int *pslave, struct winsize *winp)
{
	char* ptsName;

	if (verbose)
	{
		write_verbose("\033[31;40m{PID:%u} creating %s `/dev/ptmx`\033[m\r\n", getpid(), adescr);
	}

	if ((*pmaster = open ("/dev/ptmx", O_RDWR | O_NOCTTY)) == -1)
	{
		write_verbose("\033[30;41m\033[K{PID:%u} open(`/dev/ptmx`) failed (%i): %s\033[m\r\n", getpid(), errno, strerror(errno));
		return -1;
	}

	if (verbose)
	{
		write_verbose("\033[31;40m{PID:%u} %s handle is (%i)\033[m\r\n", getpid(), adescr, *pmaster);
	}

	if (grantpt(*pmaster) == -1)
	{
		write_verbose("\033[30;41m\033[K{PID:%u} grantpt(%i) failed (%i): %s\033[m\r\n", getpid(), *pmaster, errno, strerror(errno));
		return -1;
	}
	if (unlockpt(*pmaster) == -1)
	{
		write_verbose("\033[30;41m\033[K{PID:%u} unlockpt(%i) failed (%i): %s\033[m\r\n", getpid(), *pmaster, errno, strerror(errno));
		return -1;
	}

	ptsName = ptsname(*pmaster);
	if (ptsName == NULL)
	{
		write_verbose("\033[30;41m\033[K{PID:%u} ptsname(%i) failed (%i): %s\033[m\r\n", getpid(), *pmaster, errno, strerror(errno));
		return -1;
	}
	if (verbose)
	{
		write_verbose("\033[31;40m{PID:%u} opening slave `%s`\033[m\r\n", getpid(), ptsName);
	}

	if ((*pslave = open (ptsName, O_RDWR | O_NOCTTY)) == -1)
	{
		write_verbose("\033[30;41m\033[K{PID:%u} open(`%s`) failed (%i): %s\033[m\r\n", getpid(), ptsName, errno, strerror(errno));
		close(*pmaster);
		return -1;
	}

	if (verbose)
	{
		write_verbose("\033[31;40m{PID:%u} slave handle is (%i)\033[m\r\n", getpid(), *pslave);
	}

	if (winp)
	{
		// Allow to continue even on errors
		resize_pty(*pmaster, winp);
	}

	return 0;
}
#endif

void child_reset(int a_slave_out = STDOUT_FILENO, int a_slave_err = STDERR_FILENO)
{
	pid = 0;

	signal(SIGUSR1, sigusr1);
	gb_sigusr1 = false;

	slave_std_out = a_slave_out;
	slave_std_err = a_slave_err;

	memset(&Connector, 0, sizeof(Connector));
	fnRequestTermConnector = NULL;
}

static int ce_forkpty(int *pmaster, int *pmaster_err, struct winsize *winp)
{
	#if !defined(HAS_FORKPTY)
	int master_std = -1, slave_std = -1, master_err = -1, slave_err = -1;
	//sigset_t sset;
	#endif

	#if !defined(HAS_FORKPTY)
	if (ce_createpty("master", &master_std, &slave_std, winp) < 0)
		return -1;

	// Allow duplex mode? (it allow to distinct stderr and stdout)
	if (pmaster_err != NULL)
	{
		if (ce_createpty("stderr master", &master_err, &slave_err, winp) < 0)
			return -1;
	}
	#endif

	signal(SIGUSR1, sigusr1);
	gb_sigusr1 = false;

	if (verbose)
	{
		write_verbose("\033[31;40m\033[K{PID:%u} calling fork (pgid=%i)\033[m\r\n", getpid(), getpgrp());
	}

	errno = ENOENT;

#if defined(HAS_FORKPTY)
	pid = forkpty(pmaster, NULL, NULL, winp);
#else
	pid = fork();
#endif

	// Fork failed
	if (pid == -1)
	{
		child_err_msg("fork failed");
		return -1;
	}

	// Child process here
	if (pid == 0)
	{
		char  *stdName, *errName;
		int   newStd, newErr;
		DWORD tBegin, tEnd;

		#if defined(HAS_FORKPTY)
		int slave_std = STDOUT_FILENO, slave_err = STDERR_FILENO;
		#endif

		// To be sure we will not call these functions in child
		#if defined(HAS_FORKPTY)
		child_reset();
		#else
		child_reset(slave_std, (slave_err >= 0) ? slave_err : slave_std);
		#endif


		// Don't use logging descriptor in child
		if (gnLogFile != -1)
		{
			close(gnLogFile);
			gnLogFile = -1;
		}

		if (verbose)
		{
			write_verbose("\033[33;40m\033[K{PID:%u} child created (pgid=%i)\033[m\r\n", getpid(), getpgrp());
		}

		//if (setpgid(getpid(), getpid())
		//if (setpgrp() == -1)
		//{
		//	write_verbose("\033[30;41m\033[K{PID:%u} setpgrp failed\033[m\r\n", getpid(), getpgrp());
		//}

		if (verbose)
		{
			// Actually, terminal will not print child output until it get into run() function
			write_verbose("\033[33;40m\033[K{PID:%u} child process wating for SIGUSR1 (pgid=%i)\033[m\r\n", getpid(), getpgrp());
		}

		// Wait a little until parent process let us go
		tBegin = GetTickCount();
		sleep(5);
		tEnd = GetTickCount();
		if (verbose)
		{
			write_verbose("\033[33;40m\033[K{PID:%u} child process continues after %u ms (SIGUSR1=%u)\033[m\r\n", getpid(), (tEnd-tBegin), (gb_sigusr1?1:0));
		}

		// TODO: tty_ioctl(TIOCSPGRP?)

		#if !defined(HAS_FORKPTY)
		// Close master descriptors
		close(master_std);
		if (master_err >= 0)
			close(master_err);
		#endif

		#if !defined(HAS_FORKPTY)
		if (verbose)
		{
			write_verbose("\033[33;40m{PID:%u} ce_forkpty child: std=%i, err=%i\033[m\r\n", getpid(), slave_std, slave_err);
		}

		if (setsid() == -1)
		{
			write_verbose("\033[30;41m\033[K{PID:%u} setsid() failed (%i): %s\033[m\r\n", getpid(), errno, strerror(errno));
			return -1;
		}
		else if (verbose)
		{
			write_verbose("\033[33;40m\033[K{PID:%u} setsid executed (pgid=%i)\033[m\r\n", getpid(), getpgrp());
		}

		if (slave_err < 0)
			slave_err = slave_std;
		#endif

		stdName = ttyname(slave_std);
		errName = (slave_err == slave_std) ? stdName : ttyname(slave_err);

		if (verbose)
		{
			write_verbose("\033[33;40m{PID:%u} ttyname(%i)=`%s`\033[m\r\n", getpid(), slave_std, stdName ? stdName : "<null>");
			if (slave_err != slave_std)
				write_verbose("\033[33;40m{PID:%u} ttyname(%i)=`%s`\033[m\r\n", getpid(), slave_std, errName ? errName : "<null>");
		}

		#if !defined(HAS_FORKPTY)
		// Set descriptors to standard IN/OUT/ERR ids
		if (slave_std != STDIN_FILENO)
		{
			if (verbose)
				write_verbose("\033[33;40m{PID:%u} changing stdin: %i -> %i\033[m\r\n", getpid(), slave_std, STDIN_FILENO);
			close(STDIN_FILENO);
			dup2(slave_std, STDIN_FILENO);
		}
		else if (verbose)
		{
			write_verbose("\033[33;40m{PID:%u} stdin is already %i\033[m\r\n", getpid(), slave_std);
		}

		if (slave_std != STDOUT_FILENO)
		{
			if (verbose)
				write_verbose("\033[33;40m{PID:%u} changing stdout: %i -> %i\033[m\r\n", getpid(), slave_std, STDOUT_FILENO);
			close(STDOUT_FILENO);
			dup2(slave_std, STDOUT_FILENO);
		}
		else if (verbose)
		{
			write_verbose("\033[33;40m{PID:%u} stdout is already %i\033[m\r\n", getpid(), slave_std);
		}
		slave_std_out = STDOUT_FILENO;

		if (slave_err != STDERR_FILENO)
		{
			if (verbose)
				write_verbose("\033[33;40m{PID:%u} changing stderr: %i -> %i\033[m\r\n", getpid(), slave_err, STDERR_FILENO);
			close(STDERR_FILENO);
			dup2(slave_err, STDERR_FILENO);
		}
		else if (verbose)
		{
			write_verbose("\033[33;40m{PID:%u} stderr is already %i\033[m\r\n", getpid(), slave_err);
		}
		slave_std_err = STDERR_FILENO;

		// Close unused descriptors
		if (slave_std > STDERR_FILENO)
			close(slave_std);
		if ((slave_err != slave_std) && (slave_err > STDERR_FILENO))
			close(slave_err);
		#endif

		// Child "login into terminal" succeeded
		return 0;
	} // if (pid == 0)


	// Parent process here

	if (verbose)
	{
		write_verbose("\033[31;40m\033[K{PID:%u} child pid=%i pgid=%i was created (ourpgid=%i)\033[m\r\n", getpid(), pid, getpgid(pid), getpgrp());
	}

	#if !defined(HAS_FORKPTY)
	if (slave_std >= 0)
		close(slave_std);
	if (slave_err >= 0)
		close(slave_err);
	if (pmaster)
		*pmaster = master_std;
	if (pmaster_err)
		*pmaster_err = master_err;
	#endif

	// Fork succeeded
	return pid;
}

void create_log_file(const char* pszDir)
{
	wchar_t* pszCmdLine;

	// "[dir/]connector-%pid%.log"
	int iDirLen = pszDir ? strlen(pszDir) : 0;
	char *pszLog = (char*)malloc(iDirLen+64);
	if (iDirLen > 0)
	{
		strcpy(pszLog, pszDir);
		// add posix-way trailing slash if absent
		if (!strchr("\\/", pszDir[iDirLen-1]))
			pszLog[iDirLen++] = '/';
	}
	else
	{
		strcpy(pszLog, "./");
		iDirLen = 2;
	}
	sprintf(pszLog+iDirLen, "connector-%u.log", getpid());

	// Let's create log file...
	// umask(777); -- no need to reset?
	gnLogFile = open(pszLog, O_WRONLY|O_CREAT|O_TRUNC, 0600);
	// Succeeded?
	if (gnLogFile >= 0)
	{
		// There is some permission crazyness while creating new files
		fchmod(gnLogFile, 0600);

		// Write our full command line to first line of log-file
		if ((pszCmdLine = GetCommandLineW()) != NULL)
		{
			int wlen = lstrlenW(pszCmdLine);
			int len = WideCharToMultiByte(CP_UTF8, 0, pszCmdLine, wlen, 0, 0, 0, 0);
			if (len > 0)
			{
				char* pszUtf8 = (char*)malloc(len*sizeof(*pszUtf8));
				if (pszUtf8 && ((len = WideCharToMultiByte(CP_UTF8, 0, pszCmdLine, wlen, pszUtf8, len, 0, 0)) > 0))
				{
					write(gnLogFile, pszUtf8, len);
					write(gnLogFile, "\n----------\n", 12);
				}
				free(pszUtf8);
			}
		}
	}

	if (verbose)
		write_verbose("\r\n\033[31;40m{PID:%u} fopen(`%s`) = %i\033[m\r\n", getpid(), pszLog, gnLogFile);

	free(pszLog);
}

int main(int argc, char** argv)
{
	int iMainRc = 254;
	struct termios attr;
	const char* curTerm = NULL;
	// Another options are: xterm, xterm-256color, cygwin, msys, etc.
	const char* newTerm = "xterm-256color";
	bool force_set_term = true;
	char** cur_argv;
	bool prn_env = false;

	cur_argv = argv[0] ? argv+1 : argv;
	while (cur_argv[0])
	{
		// Last "-" means "run default shell"
		if (strcmp(cur_argv[0], "-") == 0)
		{
			cur_argv++;
			break;
		}
		// End of switches, shell command line is expected here
		if (cur_argv[0][0] != '-')
		{
			break;
		}
		// Check known switches
		if (strcmp(cur_argv[0], "--debug") == 0)
		{
			signal(SIGINT, stop_waiting_debugger);
			debugger = true;
			write_verbose("\033[31;40m{PID:%u} press Ctrl-C to stop waiting for debugger\033[m", getpid());
			for (int i = 0; (i < 60) && debugger && !IsDebuggerPresent(); i++)
			{
				sleep(1);
				write_verbose(".");
			}
			write_verbose("\033[31;40m%s\033[m\r\n", IsDebuggerPresent() ? " debugger attached" : "debugger was not attached");
		}
		else if (strcmp(cur_argv[0], "--keys") == 0)
		{
			return test_read_keys();
		}
		else if (strcmp(cur_argv[0], "--verbose") == 0)
		{
			verbose = true;
		}
		else if (strcmp(cur_argv[0], "--environ") == 0)
		{
			prn_env = true;
			pid = 0;
			print_environ(false);
		}
		else if (strcmp(cur_argv[0], "--isatty") == 0)
		{
			pid = 0;
			exit(print_isatty(true));
		}
		else if ((strcmp(cur_argv[0], "-l") == 0) || (strcmp(cur_argv[0], "--log") == 0))
		{
			// User may or may not specify directory for log files
			char* pszDir = (cur_argv[1] && (cur_argv[1][0] != '-')) ? cur_argv[1] : NULL;
			if (gnLogFile == -1)
			{
				// "[dir/]connector-%pid%.log"
				create_log_file(pszDir);
			}
			else
			{
				write_verbose("\r\n\033[31;40m{PID:%u} log file was already opened\033[m\r\n", getpid());
			}
			if (pszDir)
				cur_argv++;
		}
		else if (strcmp(cur_argv[0], "-t") == 0)
		{
			cur_argv++;
			if (!cur_argv[0])
				break;
			newTerm = cur_argv[0];
			force_set_term = true;
		}
		else if ((strcmp(cur_argv[0], "-d") == 0) || (strcmp(cur_argv[0], "--dir") == 0))
		{
			cur_argv++;
			if (!cur_argv[0])
				break;
			work_dir = cur_argv[0];
		}
		else if ((strcmp(cur_argv[0], "--shlvl") == 0))
		{
			setenv("SHLVL", "1", true);
		}
		else if ((strcmp(cur_argv[0], "--version") == 0))
		{
			pid = 0;
			print_version();
			exit(1);
		}
		else if ((strcmp(cur_argv[0], "--help") == 0) || (strcmp(cur_argv[0], "-h") == 0))
		{
			char* exe_name;
			if (argv[0])
			{
				exe_name = strrchr(argv[0], '/');
				if (exe_name) exe_name++; else exe_name = argv[0];
			}
			pid = 0;
			print_version();
			printf("Usage: %s [switches] [- | shell [shell switches]]\n", exe_name ? exe_name : "conemu-*-*.exe");
			printf("  -h, --help       this help\n");
			printf("  -d, --dir <dir>  chdir to `dir` before starting shell\n");
			printf("                   forces `set CHERE_INVOKING=1`\n");
			printf("  -t <new-term>    forces `set TERM=new-term`\n");
			printf("      --debug      wait for debugger for 60 seconds\n");
			printf("      --environ    print environment on startup\n");
			printf("      --isatty     do isatty checks and print pts names\n");
			printf("      --keys       read conin and print bare input\n");
			printf("      --shlvl      forces `set SHLVL=1` to avoid terminal reset on exit\n");
			printf("      --verbose    additional information during startup\n");
			printf("      --version    print version of this tool\n");
			exit(1);
		}
		else
		{
			write_verbose("\033[31;40m\033[K{PID:%u} Unknown switch: %s\033[m\r\n", getpid(), cur_argv[0]);
			exit(255);
		}

		// Next switch
		cur_argv++;
	}

	// Request xterm emulation in ConEmu, obtain callback functions
	if (RequestTermConnector() != 0)
	{
		exit(254);
	}

	tcgetattr(0, &attr);
	attr.c_cc[VERASE] = CDEL;
	attr.c_iflag = 0;
	attr.c_lflag = ISIG;
	tcsetattr(0, TCSANOW, &attr);

	signal(SIGHUP, SIG_IGN);

	signal(SIGINT, sigexit);
	signal(SIGTERM, sigexit);
	signal(SIGQUIT, sigexit);

	SetConsoleCtrlHandler(CtrlHandlerRoutine, true);

	winsize winp = {25, 80};
	query_console_size(&winp);

	curTerm = getenv("TERM");
	if (!curTerm || force_set_term)
	{
		if (verbose)
		{
			write_verbose("\033[31;40m{PID:%u} declaring TERM: `%s` (was: `%s`)\033[m\r\n", getpid(), newTerm, curTerm ? curTerm : "");
		}

		setenv("TERM", newTerm, true);
	}
	else if (verbose)
	{
		write_verbose("\033[31;40m{PID:%u} TERM already defined: `%s`\033[m\r\n", getpid(), curTerm);
	}

	if (verbose)
	{
		const char* curHome = getenv("HOME");
		write_verbose("\033[31;40m{PID:%u} current $HOME is `%s`\033[m\r\n", getpid(), curHome ? curHome : "<null>");
	}

	if (verbose)
	{
		print_isatty(false);
	}

	// Create the terminal instance
	pid = ce_forkpty(&pty_fd, NULL/*&pty_err*/, &winp);
	// Error in fork?
	if (pid < 0)
	{
		// If we get here, fork (CreateProcess for child connector process) was failed.
		child_err_msg("ce_forkpty failed");
		exit(253);
	}
	// Child process (going to start shell)
	else if (!pid)
	{
		child_msg_box("child process created","connector's child");
		child_reset();

		// Reset signals
		signal(SIGHUP, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGQUIT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGCHLD, SIG_DFL);

		// Mimick login's behavior by disabling the job control signals
		signal(SIGTSTP, SIG_IGN);
		signal(SIGTTIN, SIG_IGN);
		signal(SIGTTOU, SIG_IGN);

		struct termios attr;
		tcgetattr(0, &attr);
		attr.c_cc[VERASE] = CDEL;
		attr.c_iflag |= IXANY | IMAXBEL;
		attr.c_lflag |= ECHOE | ECHOK | ECHOCTL | ECHOKE;
		tcsetattr(0, TCSANOW, &attr);

		// Invoke command
		char * const def_argv[] = {"/usr/bin/bash", "-l", "-i", NULL};
		child_argv = cur_argv[0] ? cur_argv : def_argv;

		#if defined(SHOW_CHILD_ERR_MSG)
		char chMsg[255];
		sprintf(chMsg, "executing shell (WriteText:%u) (pid=%i)", Connector.WriteText?1:0, pid);
		child_msg_box(chMsg, verbose ? "connector-verbose" : "connector-non-verbose");
		#endif

		if (work_dir)
		{
			if (chdir(work_dir) == -1)
			{
				write_verbose("\033[30;41m\033[K{PID:%u} chdir `%s` failed: %s\033[m\r\n", getpid(), work_dir, strerror(errno));
			}
			else
			{
				setenv("CHERE_INVOKING", "1", true);
			}
		}

		if (verbose)
		{
			print_isatty(true);
		}

		if (prn_env)
		{
			print_environ(true);
		}

		child_msg_box("raising SIGUSR1 in parent","connector's child");

		// Inform parent "we are ready to shell"
		sigusr1_throw(getppid());
		signal(SIGUSR1, SIG_DFL);

		if (verbose)
		{
			print_shell_args();
		}

		// At least msys from GoW (1.0.17) raises `segmentation fault`
		// if it can't find shell executable, for example,
		// just "conemu-msys-32.exe bash" is not enough (SIGSEGV),
		// but "conemu-msys-32.exe /usr/bin/bash" succeeds.
		// And it succeeds if this `bin` exists in %PATH%.
		signal(SIGSEGV, sigfault);

		child_msg_box("calling execvp","connector's child");

		execvp(child_argv[0], child_argv);

		// If we get here, exec failed.
		child_err_msg("failed to run shell");
		// if we exit immediately, some versions of cygwin/msys will not be able to print our message
		sleep(1);
		exit(errno ? errno : 252);
	}
	// Parent process
	else
	{
		child_msg_box("child process created","connector");
		char *dev = ptsname(pty_fd);

		if (verbose)
		{
			write_verbose("\033[31;40m{PID:%u} PTY was created: `%s`; Child PID:%u\033[m\r\n", getpid(), dev ? dev : "<null>", pid);
		}

		if (verbose)
		{
			print_isatty(false);
		}

		fcntl(pty_fd, F_SETFL, O_NONBLOCK);

		if (dev)
		{
			struct utmp ut;
			memset(&ut, 0, sizeof ut);

			if (!strncmp(dev, "/dev/", 5))
				dev += 5;
			lstrcpyn(ut.ut_line, dev, sizeof ut.ut_line);

			if (dev[1] == 't' && dev[2] == 'y')
				dev += 3;
			else if (!strncmp(dev, "pts/", 4))
				dev += 4;
			lstrcpyn(ut.ut_id, dev, sizeof ut.ut_id);

			ut.ut_type = USER_PROCESS;
			ut.ut_pid = pid;
			ut.ut_time = time(0);
			lstrcpyn(ut.ut_user, getlogin() ?: "?", sizeof ut.ut_user);
			gethostname(ut.ut_host, sizeof ut.ut_host);
			login(&ut);

			if (prn_env)
			{
				print_environ(false);
			}
		}

		// Thaw children
		sigusr1_throw(pid);

		iMainRc = run();
	}

	StopTermConnector();

	if (verbose)
		write_verbose("\r\n\033[31;40m{PID:%u} normal exit from main\033[m\r\n", getpid());
	return iMainRc;
}
