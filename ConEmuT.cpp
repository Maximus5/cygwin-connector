
// from https://github.com/Alexpux/MSYS2-packages/issues/265

#undef _USE_DEBUG_LOG_INPUT

#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <stdarg.h>
#include <errno.h>
#include <process.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/wait.h>
#include <sys/select.h>

#include <w32api/wtypes.h>
#include <w32api/wincon.h>
#include <w32api/winuser.h>

#include <unistd.h>
#include <utmp.h>

#define _max(a,b) (((a) > (b)) ? (a) : (b))

bool verbose = false;
bool debugger = false;
static void write_verbose(const char *buf, ...);
static void print_version();

#include "version.h"
#include "forkpty.h"


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

static RequestTermConnectorParm Connector = {};

typedef int (WINAPI* RequestTermConnector_t)(/*[IN/OUT]*/RequestTermConnectorParm* Parm);
static RequestTermConnector_t fnRequestTermConnector = NULL;

static int RequestTermConnector()
{
	int iRc;
	HMODULE hConEmuHk;
	char sModule[] =
		#if defined(__x86_64__)
			"ConEmuHk64.dll"
		#else
			"ConEmuHk.dll"
		#endif
			;

	hConEmuHk = GetModuleHandleA(sModule);
	if (hConEmuHk == NULL)
	{
		write_verbose("\r\n\033[31;40m{PID:%u} %s is not found, exiting\033[m\r\n", getpid(), sModule);
		return -1;
	}
	fnRequestTermConnector = (RequestTermConnector_t)GetProcAddress(hConEmuHk, "RequestTermConnector");
	if (fnRequestTermConnector == NULL)
	{
		write_verbose("\r\n\033[31;40m{PID:%u} RequestTermConnector function is not found, exiting\033[m\r\n", getpid());
		return -1;
	}

	// Prepare arguments
	memset(&Connector, 0, sizeof(Connector));
	Connector.cbSize = sizeof(Connector);
	Connector.Mode = rtc_Start;
	Connector.pszTtyName = ttyname(STDOUT_FILENO);
	Connector.pszTerm = getenv("TERM");

	iRc = fnRequestTermConnector(&Connector);

	if (iRc != 0)
	{
		write_verbose("\r\n\033[31;40m{PID:%u} RequestTermConnector failed (%i). %s\033[m\r\n", getpid(), iRc, Connector.pszError ? Connector.pszError : "");
		return -1;
	}

	if (!Connector.ReadInput || !Connector.WriteText)
	{
		write_verbose("\r\n\033[31;40m{PID:%u} RequestTermConnector returned NULL. %s\033[m\r\n", getpid(), Connector.pszError ? Connector.pszError : "");
		return -1;
	}

	return 0;
}

static void StopTermConnector()
{
	if (fnRequestTermConnector)
	{
		Connector.cbSize = sizeof(Connector);
		Connector.Mode = rtc_Stop;
		fnRequestTermConnector(&Connector);
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



static int pty_fd = -1;
static pid_t pid = -1;
static HANDLE input_thread = NULL;
static DWORD input_tid = 0;
static void stop_threads();
static bool termination = false;

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
			bRc = Connector.WriteText(buf, len, &written, wps_Output);
		}
		else if (pid > 0)
		{
			// Server side, before initialization
			// We need to call API directly, because fwrite/printf/...
			// may break colors, if they were written in wrong moment
			bRc = WriteConsoleA(GetStdHandle((strm == wps_Output) ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE), buf, len, &written, NULL);
		}
		else
		{
			// Child (client) side
			written = fwrite(buf, 1, len, (strm == wps_Output) ? stdout : stderr);
			bRc = (written != 0);
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

static int resize_pty(int pty, const struct winsize *winp)
{
	int iRc = -99;

	if (pty != -1)
	{
		// SIGWINCH signal is sent to the foreground process group
		iRc = ioctl(pty, TIOCSWINSZ, winp);

		if (verbose)
		{
			if (iRc == -1)
				write_verbose("\033[31;40m{PID:%u} ioctl(%i,TIOCSWINSZ,(%i,%i)) failed (%i): %s\033[m\r\n", getpid(), pty, winp->ws_col, winp->ws_row, errno, strerror(errno));
			else
				write_verbose("\033[31;40m{PID:%u} ioctl(%i,TIOCSWINSZ,(%i,%i)) succeeded (%i)\033[m\r\n", getpid(), pty, winp->ws_col, winp->ws_row, iRc);
		}
	}

	return iRc;
}

static bool query_console_size(struct winsize* winp)
{
	memset(winp, 0, sizeof(winp));
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};
	if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
	{
		winp->ws_row = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
		winp->ws_col = csbi.dwSize.X;
	}
	else
	{
		winp->ws_row = 25;
		winp->ws_col = 80;
	}
	winp->ws_xpixel = winp->ws_col * 3;
	winp->ws_ypixel = winp->ws_row * 5;
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
						if (pty_fd != -1)
							resize_pty(pty_fd, &winp);
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
		}
		else if (pid > 0)
		{
			int status;
			debug_log_format("%u:PID=%u:TID=%u: calling waitpid(%i)\n", GetTickCount(), getpid(), GetCurrentThreadId(), pid);
			if (waitpid(pid, &status, WNOHANG) == pid)
			{
				if (verbose)
				{
					write_verbose("\r\n\033[31;40m{PID:%u} pid=%i was terminated\033[m\r\n", getpid(), pid);
				}

				pid = 0;
				break;
			}
			else // Pty gone, but process still there: keep checking
				timeout_p = &timeout;
			debug_log_format("%u:PID=%u:TID=%u: waitpid(%i) done\n", GetTickCount(), getpid(), GetCurrentThreadId(), pid);
		}

		const int fdsmax = pty_fd + 1;
		debug_log_format("%u:PID=%u:TID=%u: calling select\n", GetTickCount(), getpid(), GetCurrentThreadId());
		timeout.tv_usec = 100000;
		if (select(fdsmax, &fds, 0, 0, timeout_p) > 0)
		{
			if (pty_fd >= 0 && FD_ISSET(pty_fd, &fds))
			{
				debug_log_format("%u:PID=%u:TID=%u: calling read(%i)\n", GetTickCount(), getpid(), GetCurrentThreadId(), pty_fd);
				int len = read(pty_fd, buf, bufCount);

				if (len > 0)
				{
					while ((len+4) < preferredCount)
					{
						int addLen = read(pty_fd, buf+len, bufCount-len);
						if (addLen <= 0)
							break;
						len += addLen;
					}
					buf[len] = 0;
					write_console(buf, len);
				}
				else
				{
					pty_fd = -1;
				}
			}
		}
		else
		{
			debug_log_format("%u:PID=%u:TID=%u: select failed\n", GetTickCount(), getpid(), GetCurrentThreadId());
		}
	}

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
	int iTty, errNo;
	char* ttyName;

	for (int f = STDIN_FILENO; f <= STDERR_FILENO; f++)
	{
		ttyName = ttyname(f);
		iTty = isatty(f);
		errNo = errno;
		if (iTty == 1)
		{
			write_verbose("\033[32;40m{PID:%u} %i: isatty()=%i; ttyname()=`%s`\033[m\n", getpid(), f, iTty, ttyName?ttyName:"<NULL>");
		}
		else
		{
			write_verbose("\033[31;40m{PID:%u} %i: isatty()=%i; errno=%i; ttyname()=`%s`\033[m\n", getpid(), f, iTty, errNo, ttyName?ttyName:"<NULL>");
			isTty = false;
		}
	}

	return isTty ? 0 : 1;
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
	const char* work_dir = NULL;
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
			print_environ(false);
		}
		else if (strcmp(cur_argv[0], "--isatty") == 0)
		{
			exit(print_isatty(true));
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
		else if ((strcmp(cur_argv[0], "--version") == 0))
		{
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
			print_version();
			printf("Usage: %s [switches] [- | shell [shell switches]]\n", exe_name ? exe_name : "conemu-*-*.exe");
			printf("  -h, --help       this help\n");
			printf("  -d, --dir <dir>  chdir to `dir` before starting shell\n");
			printf("                   forces `set CHERE_INVOKING=1`\n");
			printf("  -t <new-term>    forced set `TERM` variable to `new-term`\n");
			printf("      --keys       read conin and print bare input\n");
			printf("      --verbose    additional information during startup\n");
			printf("      --version    print version of this tool\n");
			printf("      --debug      wait for debugger for 60 seconds\n");
			printf("      --environ    print environment on startup\n");
			printf("      --isatty     fo isatty checks and print pts names\n");
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
		exit(200);
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
		write_verbose("\r\n\033[31;40m{PID:%u} current $HOME is `%s`\033[m\r\n", getpid(), curHome ? curHome : "<null>");
	}

	if (verbose)
	{
		print_isatty(false);
	}

	// Create the terminal instance
	pid = ce_forkpty(&pty_fd, &winp);
	// Error in fork?
	if (pid < 0)
	{
		// If we get here, exec failed.
		write_verbose("\033[30;41m\033[K{PID:%u} forkpty failed: %s\033[m\r\n", getpid(), strerror(errno));
		exit(255);
	}
	// Child process (going to start shell)
	else if (!pid)
	{
		// To be sure we will not call these functions in child
		memset(&Connector, 0, sizeof(Connector));
		fnRequestTermConnector = NULL;

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
		char * const def_argv[] = {"/usr/bin/sh", "-l", "-i", NULL};
		char * const * child_argv = cur_argv[0] ? cur_argv : def_argv;

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

		if (verbose)
		{
			char* cwd = work_dir ? NULL : getcwd(NULL, 0);
			write_verbose("\033[31;40m{PID:%u} Starting shell: `%s` in `%s`\033[m\r\n", getpid(), child_argv[0], work_dir ? work_dir : cwd ? cwd : "<%cd%>");
			free(cwd);
		}

		// sleep(2);

		if (prn_env)
		{
			print_environ(true);
		}

		execvp(child_argv[0], child_argv);

		// If we get here, exec failed.
		write_verbose("\033[30;41m\033[K{PID:%u} Failed to run %s: %s\033[m\r\n", getpid(), child_argv[0], strerror(errno));

		exit(255);
	}
	// Parent process
	else
	{
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
		iMainRc = run();
	}

	StopTermConnector();

	if (verbose)
		write_verbose("\r\n\033[31;40m{PID:%u} normal exit from main\033[m\r\n", getpid());
	return iMainRc;
}
