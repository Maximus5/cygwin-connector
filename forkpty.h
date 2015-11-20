#if defined(__MSYS__) && (__GNUC__ <= 3)

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

#define TTY_NAME_MAX 32

int
login_tty(int fd)
{
  char *fdname;
  int newfd;

  if (setsid () == -1)
    return -1;
  if ((fdname = ttyname (fd)))
    {
      if (fd != STDIN_FILENO)
        close (STDIN_FILENO);
      if (fd != STDOUT_FILENO)
        close (STDOUT_FILENO);
      if (fd != STDERR_FILENO)
        close (STDERR_FILENO);
      newfd = open (fdname, O_RDWR);
      close (newfd);
    }
  dup2 (fd, STDIN_FILENO);
  dup2 (fd, STDOUT_FILENO);
  dup2 (fd, STDERR_FILENO);
  if (fd > 2)
    close (fd);
  return 0;
}

int
openpty(int *amaster, int *aslave, char *name,
        const struct termios *termp, const struct winsize *winp)
{
  int master, slave;
  char pts[TTY_NAME_MAX];

  if ((master = open ("/dev/ptmx", O_RDWR | O_NOCTTY)) >= 0)
    {
      grantpt (master);
      unlockpt (master);
      strcpy (pts, ptsname (master));
      if ((slave = open (pts, O_RDWR | O_NOCTTY)) >= 0)
        {
          if (amaster)
            *amaster = master;
          if (aslave)
            *aslave = slave;
          if (name)
            strcpy (name, pts);
          if (termp)
            tcsetattr (slave, TCSAFLUSH, termp);
          if (winp)
            ioctl (master, TIOCSWINSZ, (char *) winp);
          return 0;
        }
      close (master);
    }
  errno = ENOENT;
  return -1;
}

int
forkpty(int *amaster, char *name,
        const struct termios *termp, const struct winsize *winp)
{
  int master, slave, pid;

  if (openpty (&master, &slave, name, termp, winp) == -1)
    return -1;
  switch (pid = fork ())
    {
      case -1:
        return -1;
      case 0:
        close (master);
        login_tty (slave);
        return 0;
    }
  if (amaster)
    *amaster = master;
  close (slave);
  return pid;
}

#endif
