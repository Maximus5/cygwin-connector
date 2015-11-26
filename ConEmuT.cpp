
// from https://github.com/Alexpux/MSYS2-packages/issues/265

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

#if defined(__MSYS__) && (__GNUC__ <= 3)

#include "forkpty.h"
#define _max max

#else

#include <w32api/processenv.h>
#include <w32api/apisetcconv.h>
#include <pty.h>
#define _max std::max

#endif

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

static void sigexit(int sig)
{
	debug_log_format("signal %i received, pid=%i\n", sig, pid);
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
		INPUT_RECORD r = {}; DWORD nReady = 0;
		if (ReadConsoleInputW(h_input, &r, 1, &nReady) && nReady)
		{
			debug_log_format("read_input_thread: event %u received\n", r.EventType);
			switch (r.EventType)
			{
			case WINDOW_BUFFER_SIZE_EVENT:
				{
					debug_log_format("read_input_thread: WindowBufferSize={%i,%i}\n", r.Event.WindowBufferSizeEvent.dwSize.X, r.Event.WindowBufferSizeEvent.dwSize.Y);
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
						#if defined(_USE_DEBUG_LOG)
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
	char buf[4096];
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
			FD_SET(pty_fd, &fds);
		else if (pid)
		{
			int status;
			if (waitpid(pid, &status, WNOHANG) == pid)
			{
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
				int len = read(pty_fd, buf, sizeof buf);

				if (len > 0)
				{
					write_console(buf, len);
				}
				else
				{
					pty_fd = -1;
				}
			}
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

int main(int argc, char** argv)
{
	struct termios attr;
	tcgetattr(0, &attr);
	attr.c_cc[VERASE] = CDEL;
	attr.c_iflag = 0;
	attr.c_lflag = ISIG;
	tcsetattr(0, TCSANOW, &attr);

	signal(SIGHUP, SIG_IGN);

	signal(SIGINT, sigexit);
	signal(SIGTERM, sigexit);
	signal(SIGQUIT, sigexit);

	winsize winp = {25, 80};
	query_console_size(&winp);

	if (!getenv("TERM"))
		setenv("TERM", "xterm-256color", true);
	//#if defined(__MSYS__)
	//setenv("TERM", "msys", true);
	//#else
	//setenv("TERM", "cygwin", true);
	//#endif

	SetConsoleCP(65001);
	SetConsoleOutputCP(65001);

	pid = forkpty(&pty_fd, 0, 0, &winp);
	if (pid < 0)
	{

	}
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
		char * const * child_argv = def_argv;

		if (argv[0] && argv[1] && strcmp(argv[1], "-"))
			child_argv = argv+1;

		// sleep(2);
		execvp(child_argv[0], child_argv);

		// If we get here, exec failed.
		fprintf(stderr, "\033[30;41m\033[KFailed to run %s: %s\033[m\r\n", child_argv[0], strerror(errno));

		exit(255);
	}
	else
	{
		fcntl(pty_fd, F_SETFL, O_NONBLOCK);

		char *dev = ptsname(pty_fd);
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

	return 0;
}
