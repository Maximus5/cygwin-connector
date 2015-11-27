
/*-
 * Copyright (c) 1990, 1993
 *      The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * CV 2003-09-10: Cygwin specific changes applied.  Code simplified just
 *                for Cygwin alone.
 */

#include <fcntl.h>
#include <sys/termios.h>
#include <sys/ioctl.h>
#include <sys/errno.h>

int ce_login_tty(int fd)
{
	char* fdname;
	int   newfd;
	char  log_buf[200];

	if (verbose)
	{
		snprintf(log_buf, sizeof log_buf, "\033[31;40m{PID:%u} ce_login_tty(%i), calling setsid\033[m\r\n", getpid(), fd);
		write_verbose(log_buf);
	}

	if (setsid() == -1)
	{
		fprintf(stderr, "\033[30;41m\033[K{PID:%u} setsid() failed (%i): %s\033[m\r\n", getpid(), errno, strerror(errno));
		return -1;
	}

	if ((fdname = ttyname(fd)))
	{
		if (verbose)
		{
			snprintf(log_buf, sizeof log_buf, "\033[31;40m{PID:%u} preparing stdin(%i), stdout(%i), strerr(%i) for '", getpid(), STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);
			write_verbose(log_buf);
			write_verbose(fdname);
			write_verbose("'\033[m\r\n");
		}

		if (fd != STDIN_FILENO)
			close (STDIN_FILENO);
		if (fd != STDOUT_FILENO)
			close (STDOUT_FILENO);
		if (fd != STDERR_FILENO)
			close (STDERR_FILENO);

		newfd = open (fdname, O_RDWR);
		close (newfd);
	}
	else if (verbose)
	{
		snprintf(log_buf, sizeof log_buf, "\033[31;40m{PID:%u} ttyname(%i) returned null\033[m\r\n", getpid(), fd);
		write_verbose(log_buf);
	}

	dup2 (fd, STDIN_FILENO);
	dup2 (fd, STDOUT_FILENO);
	dup2 (fd, STDERR_FILENO);

	if (fd > 2)
		close (fd);

	return 0;
}

int ce_openpty(int *amaster, int *aslave,
               const struct termios *termp,
               const struct winsize *winp)
{
	int   master, slave;
	char* pts;
	char  log_buf[200];

	if (verbose)
	{
		snprintf(log_buf, sizeof log_buf, "\033[31;40m{PID:%u} creating master '/dev/ptmx'\033[m\r\n", getpid());
		write_verbose(log_buf);
	}

	if ((master = open ("/dev/ptmx", O_RDWR | O_NOCTTY)) >= 0)
	{
		if (verbose)
		{
			snprintf(log_buf, sizeof log_buf, "\033[31;40m{PID:%u} master handle is (%i)\033[m\r\n", getpid(), master);
			write_verbose(log_buf);
		}

		grantpt(master);
		unlockpt(master);

		pts = ptsname(master);
		if (verbose)
		{
			snprintf(log_buf, sizeof log_buf, "\033[31;40m{PID:%u} opening slave '", getpid());
			write_verbose(log_buf);
			write_verbose(pts ? pts : "<null>");
			write_verbose("'\033[m\r\n");
		}

		if ((slave = open (pts, O_RDWR | O_NOCTTY)) >= 0)
		{
			if (verbose)
			{
				snprintf(log_buf, sizeof log_buf, "\033[31;40m{PID:%u} slave handle is (%i)\033[m\r\n", getpid(), slave);
				write_verbose(log_buf);
			}

			if (amaster)
				*amaster = master;
			if (aslave)
				*aslave = slave;
			if (termp)
				tcsetattr(slave, TCSAFLUSH, termp);
			if (winp)
				ioctl(master, TIOCSWINSZ, (char *) winp);
			return 0;
		}

		fprintf(stderr, "\033[30;41m\033[K{PID:%u} open('%s') failed (%i): %s\033[m\r\n", getpid(), pts ? pts : "<null>", errno, strerror(errno));
		close (master);
	}
	else
	{
		fprintf(stderr, "\033[30;41m\033[K{PID:%u} ce_openpty('/dev/ptmx') failed (%i): %s\033[m\r\n", getpid(), errno, strerror(errno));
	}

	errno = ENOENT;
	return -1;
}

int ce_forkpty(int *amaster, const struct winsize *winp)
{
	int master, slave, pid;

	if (ce_openpty(&master, &slave, NULL, winp) == -1)
		return -1;

	switch ((pid = fork()))
	{
	case -1:
		fprintf(stderr, "\033[30;41m\033[K{PID:%u} fork failed (%i): %s\033[m\r\n", getpid(), errno, strerror(errno));
		return -1;
	case 0:
		close(master);
		if (ce_login_tty(slave) < 0)
		{
			fprintf(stderr, "\033[30;41m\033[K{PID:%u} ce_login_tty(%i) failed (%i): %s\033[m\r\n", getpid(), slave, errno, strerror(errno));
		}
		return 0;
	}

	if (amaster)
		*amaster = master;
	close(slave);
	return pid;
}
