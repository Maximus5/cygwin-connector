
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

static void debug_log(const char* text)
{
	#if defined(_USE_DEBUG_LOG)
	OutputDebugStringA(text);
	#endif
}
static void debug_log_format(const char* format,...)
{
	#if defined(_USE_DEBUG_LOG)
	va_list ap;
	char buf[255];
	va_start(ap, format);
	vsnprintf(buf, sizeof buf, format, ap);
	va_end(ap);
	debug_log(buf);
	#endif
}



static int pty_fd = -1;
static pid_t pid;
static HANDLE input_thread = NULL;
static DWORD input_tid = 0;
static void stop_threads();
static bool termination = false;

// If ENABLE_PROCESSED_INPUT is set, cygwin application are terminated without opportunity to survive
DWORD ProtectCtrlBreakTrap(HANDLE h_input = GetStdHandle(STD_INPUT_HANDLE))
{
	DWORD conInMode = 0;
	if (GetConsoleMode(h_input, &conInMode) && (conInMode & ENABLE_PROCESSED_INPUT))
	{
		if (verbose)
			write_verbose("\033[31;40m{PID:%u,TID:%u} dropping ENABLE_PROCESSED_INPUT flag\033[m\r\n", getpid(), GetCurrentThreadId());
		SetConsoleMode(h_input, (conInMode & ~ENABLE_PROCESSED_INPUT));
	}
	return conInMode;
}

BOOL WINAPI CtrlHandlerRoutine(DWORD dwCtrlType)
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
		if (pid)
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
		//if (pid)
		//	kill(pid, sig); // or kill(-group, sig)
		return;
	}

	if (pid)
		kill(-pid, SIGHUP);
	stop_threads();
	signal(sig, SIG_DFL);
	kill(getpid(), sig);
}

static bool write_console(const char *buf, int len)
{
	if (len == -1)
		len = strlen(buf);

	while (len > 0)
	{
		DWORD written = 0;
		if (!WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), buf, len, &written, NULL))
		{
			return false;
		}
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
	write_console((ilen > 0) ? szBuf : buf, -1);
}

static void child_resize(struct winsize *winp)
{
	if (pty_fd >= 0)
		ioctl(pty_fd, TIOCSWINSZ, winp);
}

static bool query_console_size(struct winsize* winp)
{
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

static bool read_console(int realConIn, char *buf, const int len)
{
	debug_log_format("read_console: calling read on %i\n", realConIn);
	ssize_t c = read(realConIn, buf, len);
	while (c > 0)
	{
		ssize_t written = write(pty_fd, buf, c);
		debug_log_format("read_console: writing %i bytes, written %i bytes\n", c, written);
		c -= written;
		buf += written;
	}
	return true;
}

static DWORD WINAPI read_input_thread( void * )
{
	HANDLE h_input = GetStdHandle(STD_INPUT_HANDLE);
	while (!termination)
	{
		ProtectCtrlBreakTrap(h_input);

		INPUT_RECORD r = {}; DWORD nReady = 0;
		if (ReadConsoleInputW(h_input, &r, 1, &nReady) && nReady)
		{
			#if defined(_USE_DEBUG_LOG_INPUT)
			debug_log_format("read_input_thread: event %u received\n", r.EventType);
			#endif
			switch (r.EventType)
			{
			case WINDOW_BUFFER_SIZE_EVENT:
				{
					#if defined(_USE_DEBUG_LOG_INPUT)
					debug_log_format("read_input_thread: WindowBufferSize={%i,%i}\n", r.Event.WindowBufferSizeEvent.dwSize.X, r.Event.WindowBufferSizeEvent.dwSize.Y);
					#endif
					winsize winp;
					if (query_console_size(&winp))
						child_resize(&winp);
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

	if (input_thread && (WaitForSingleObject(input_thread,0) == WAIT_TIMEOUT))
	{
		HANDLE h_input = GetStdHandle(STD_INPUT_HANDLE);
		INPUT_RECORD r = {KEY_EVENT}; DWORD nWritten = 0;
		WriteConsoleInputW(h_input, &r, 1, &nWritten);
		if (WaitForSingleObject(input_thread, 5000) == WAIT_TIMEOUT)
		{
			TerminateThread(input_thread, 100);
		}
	}
}

static int run()
{
	fd_set fds;
	char buf[4096+1];
	struct timeval timeout = {0, 100000}, *timeout_p = 0;
	int realConIn = -1;

	input_thread = CreateThread(NULL, 0, read_input_thread, NULL, 0, &input_tid);

	if (!input_thread || (input_thread == INVALID_HANDLE_VALUE))
	{
		realConIn = open("/dev/conin", O_RDONLY);
		if (realConIn == -1)
			printf("Failed to open console input: /dev/conin\n");
		fcntl(realConIn, F_SETFL, O_NONBLOCK);
	}

	// Request xterm keyboard emulation in ConEmu
	write_console("\033]9;10\007", -1);

	for (;;)
	{
		FD_ZERO(&fds);
		if (pty_fd >= 0)
		{
			FD_SET(pty_fd, &fds);
		}
		else if (pid)
		{
			int status;
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
		}

		if (realConIn >= 0)
			FD_SET(realConIn, &fds);
		const int fdsmax = _max(pty_fd, realConIn) + 1;
		debug_log("run: calling select\n");
		if (select(fdsmax, &fds, 0, 0, timeout_p) > 0)
		{
			if (pty_fd >= 0 && FD_ISSET(pty_fd, &fds))
			{
				debug_log("run: pty_fd has data\n");
				int len = read(pty_fd, buf, sizeof(buf)-1);

				if (len > 0)
				{
					buf[len] = 0;
					write_console(buf, len);
				}
				else
				{
					pty_fd = -1;
				}
			}
			// The following condition must not be true
			else if (realConIn >= 0 && FD_ISSET(realConIn, &fds))
			{
				debug_log("run: con_in has data\n");
				read_console(realConIn, buf, sizeof buf);
			}
		}
	}

	stop_threads();

	return 0;
}

// switch `--keys` useful to check keyboard translations
int test_read_keys()
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

int main(int argc, char** argv)
{
	struct termios attr;
	const char* curTerm = NULL;
	// Another options are: xterm, xterm-256color, cygwin, msys, etc.
	const char* newTerm = "xterm";
	bool force_set_term = false;
	char** cur_argv;
	const char* work_dir = NULL;
	DWORD conInMode = 0;
	UINT curCP = 0;

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
		else if (strcmp(cur_argv[0], "-t") == 0)
		{
			cur_argv++;
			if (!cur_argv[0])
				break;
			newTerm = cur_argv[0];
			force_set_term = true;
		}
		else if (strcmp(cur_argv[0], "-d") == 0)
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
			printf("  -d <work-dir>    chdir to `work-dir` before starting shell\n");
			printf("                   forces `set CHERE_INVOKING=1`\n");
			printf("  -t <new-term>    forced set `TERM` variable to `new-term`\n");
			printf("      --keys       read conin and print bare input\n");
			printf("      --verbose    additional information during startup\n");
			printf("      --version    print version of this tool\n");
			printf("      --debug      wait for debugger for 60 seconds\n");
			exit(1);
		}
		else
		{
			fprintf(stderr, "\033[31;40m\033[K{PID:%u} Unknown switch: %s\033[m\r\n", getpid(), cur_argv[0]);
			exit(255);
		}

		// Next switch
		cur_argv++;
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

	if (!((conInMode = ProtectCtrlBreakTrap()) & ENABLE_PROCESSED_INPUT))
	{
		if (verbose)
			write_verbose("\033[31;40m{PID:%u} Flag ENABLE_PROCESSED_INPUT was not set\033[m\r\n", getpid());
	}

	winsize winp = {25, 80};
	query_console_size(&winp);

	if (!(curTerm = getenv("TERM")) || force_set_term)
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

	curCP = GetConsoleCP();
	if (curCP != 65001)
	{
		if (verbose)
			write_verbose("\r\n\033[31;40m{PID:%u} changing console CP from %u to utf-8\033[m\r\n", getpid(), curCP);
		SetConsoleCP(65001);
		SetConsoleOutputCP(65001);
	}

	if (verbose)
	{
		const char* curHome = getenv("HOME");
		write_verbose("\r\n\033[31;40m{PID:%u} current $HOME is `%s`\033[m\r\n", getpid(), curHome ? curHome : "<null>");
	}


	// Create the terminal instance
	pid = ce_forkpty(&pty_fd, &winp);
	// Error in fork?
	if (pid < 0)
	{
		// If we get here, exec failed.
		fprintf(stderr, "\033[30;41m\033[K{PID:%u} forkpty failed: %s\033[m\r\n", getpid(), strerror(errno));
		exit(255);
	}
	// Child process (going to start shell)
	else if (!pid)
	{
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
				fprintf(stderr, "\033[30;41m\033[K{PID:%u} chdir `%s` failed: %s\033[m\r\n", getpid(), work_dir, strerror(errno));
			}
			else
			{
				setenv("CHERE_INVOKING", "1", true);
			}
		}

		if (verbose)
		{
			char* cwd = work_dir ? NULL : getcwd(NULL, 0);
			fprintf(stdout, "\033[31;40m{PID:%u} Starting shell: `%s` in `%s`\033[m\r\n", getpid(), child_argv[0], work_dir ? work_dir : cwd ? cwd : "<%cd%>");
			free(cwd);
		}

		// sleep(2);
		execvp(child_argv[0], child_argv);

		// If we get here, exec failed.
		fprintf(stderr, "\033[30;41m\033[K{PID:%u} Failed to run %s: %s\033[m\r\n", getpid(), child_argv[0], strerror(errno));

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
		}
		run();
	}

	if (conInMode)
	{
		if (verbose)
			write_verbose("\r\n\033[31;40m{PID:%u} reverting ConInMode to 0x%08X\033[m\r\n", getpid(), conInMode);
		SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), conInMode);
	}

	if (GetConsoleCP() != curCP)
	{
		if (verbose)
			write_verbose("\r\n\033[31;40m{PID:%u} reverting console CP from %u to %u\033[m\r\n", getpid(), GetConsoleCP(), curCP);
		SetConsoleCP(curCP);
		SetConsoleOutputCP(curCP);
	}

	if (verbose)
		write_verbose("\r\n\033[31;40m{PID:%u} normal exit from main\033[m\r\n", getpid());
	return 0;
}
