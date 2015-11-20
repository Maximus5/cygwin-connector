
// from https://github.com/Alexpux/MSYS2-packages/issues/265

#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

//#if defined(__MINGW32__)
//	#include <sys/types.h>
//	#include <sys/fcntl.h>
//	//#include <sys/wait.h>
//	//#include <sys/select.h>
//
//	#include <wtypes.h>
//	//#include <apisetcconv.h>
//	#include <wincon.h>
//	//#include <processenv.h>
//	#include <winuser.h>
//
//	#include <pty.h>
//	#include <unistd.h>
//	#include <utmp.h>
//#else
	#include <sys/types.h>
	#include <sys/fcntl.h>
	//#include <sys/wait.h>
	#include <wait.h>
	#include <sys/select.h>

	#include <w32api/wtypes.h>
	#include <w32api/apisetcconv.h>
	#include <w32api/wincon.h>
	#include <w32api/processenv.h>
	#include <w32api/winuser.h>

	#include <pty.h>
	#include <unistd.h>
	#include <utmp.h>
//#endif


static int pty_fd = -1;
static pid_t pid;

static void sigexit(int sig)
{
	if (pid)
		kill(-pid, SIGHUP);
	signal(sig, SIG_DFL);
	kill(getpid(), sig);
}

static bool write_console(const char *buf, int len)
{
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

static bool read_console(int realConIn, char *buf, const int len)
{
	ssize_t c = read(realConIn, buf, len);
	while (c > 0)
	{
		ssize_t written = write(pty_fd, buf, c);
		c -= written;
		buf += written;
	}
	return true;
}

static int run()
{
	fd_set fds;
	char buf[4096];
	struct timeval timeout = {0, 100000}, *timeout_p = 0;

	int realConIn = open("/dev/console", O_RDONLY);
	fcntl(realConIn, F_SETFL, O_NONBLOCK);

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

				return 0;
			}
			else // Pty gone, but process still there: keep checking
				timeout_p = &timeout;
		}

		FD_SET(realConIn, &fds);
		const int fdsmax = std::max(pty_fd, realConIn) + 1;
		if (select(fdsmax, &fds, 0, 0, timeout_p) > 0)
		{
			if (pty_fd >= 0 && FD_ISSET(pty_fd, &fds))
			{
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
				read_console(realConIn, buf, sizeof buf);
			}
		}
	}

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
	CONSOLE_SCREEN_BUFFER_INFO csbi = {};
	if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
	{
		winp.ws_row = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
		winp.ws_col = csbi.dwSize.X;
	}
	winp.ws_xpixel = winp.ws_col * 3;
	winp.ws_ypixel = winp.ws_row * 5;

	setenv("TERM", "cygwin", true);

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

		sleep(2);
		execvp(child_argv[0], child_argv);
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
			strlcpy(ut.ut_line, dev, sizeof ut.ut_line);

			if (dev[1] == 't' && dev[2] == 'y')
				dev += 3;
			else if (!strncmp(dev, "pts/", 4))
				dev += 4;
			strncpy(ut.ut_id, dev, sizeof ut.ut_id);

			ut.ut_type = USER_PROCESS;
			ut.ut_pid = pid;
			ut.ut_time = time(0);
			strlcpy(ut.ut_user, getlogin() ?: "?", sizeof ut.ut_user);
			gethostname(ut.ut_host, sizeof ut.ut_host);
			login(&ut);
		}
		run();
	}

	return 0;
}
