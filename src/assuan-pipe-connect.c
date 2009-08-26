/* assuan-pipe-connect.c - Establish a pipe connection (client) 
   Copyright (C) 2001-2003, 2005-2007, 2009 Free Software Foundation, Inc.

   This file is part of Assuan.

   Assuan is free software; you can redistribute it and/or modify it
   under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation; either version 2.1 of
   the License, or (at your option) any later version.

   Assuan is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#ifndef HAVE_W32_SYSTEM
#include <sys/wait.h>
#else
#include <windows.h>
#endif

#include "assuan-defs.h"

/* Hacks for Slowaris.  */
#ifndef PF_LOCAL
# ifdef PF_UNIX
#  define PF_LOCAL PF_UNIX
# else
#  define PF_LOCAL AF_UNIX
# endif
#endif
#ifndef AF_LOCAL
# define AF_LOCAL AF_UNIX
#endif


#ifdef _POSIX_OPEN_MAX
#define MAX_OPEN_FDS _POSIX_OPEN_MAX
#else
#define MAX_OPEN_FDS 20
#endif


/* This should be called to make sure that SIGPIPE gets ignored.  */
static void
fix_signals (void)
{
#ifndef _ASSUAN_NO_FIXED_SIGNALS
#ifndef HAVE_DOSISH_SYSTEM  /* No SIGPIPE for these systems.  */
  static int fixed_signals;

  if (!fixed_signals)
    { 
      struct sigaction act;
        
      sigaction (SIGPIPE, NULL, &act);
      if (act.sa_handler == SIG_DFL)
	{
	  act.sa_handler = SIG_IGN;
	  sigemptyset (&act.sa_mask);
	  act.sa_flags = 0;
	  sigaction (SIGPIPE, &act, NULL);
        }
      fixed_signals = 1;
      /* FIXME: This is not MT safe */
    }
#endif /*HAVE_DOSISH_SYSTEM*/
#endif /*!_ASSUAN_NO_FIXED_SIGNALS*/
}


#ifndef HAVE_W32_SYSTEM
static int
writen (int fd, const char *buffer, size_t length)
{
  while (length)
    {
      int nwritten = write (fd, buffer, length);
      
      if (nwritten < 0)
        {
          if (errno == EINTR)
            continue;
          return -1; /* write error */
        }
      length -= nwritten;
      buffer += nwritten;
    }
  return 0;  /* okay */
}
#endif

static int
do_finish (assuan_context_t ctx)
{
  if (ctx->inbound.fd != ASSUAN_INVALID_FD)
    {
      _assuan_close (ctx->inbound.fd);
      if (ctx->inbound.fd == ctx->outbound.fd)
        ctx->outbound.fd = ASSUAN_INVALID_FD;
      ctx->inbound.fd = ASSUAN_INVALID_FD;
    }
  if (ctx->outbound.fd != ASSUAN_INVALID_FD)
    {
      _assuan_close (ctx->outbound.fd);
      ctx->outbound.fd = ASSUAN_INVALID_FD;
    }
  if (ctx->pid != (pid_t)(-1) && ctx->pid)
    {
#ifndef HAVE_W32_SYSTEM
#ifndef _ASSUAN_USE_DOUBLE_FORK
      if (!ctx->flags.no_waitpid)
        _assuan_waitpid (ctx->pid, NULL, 0); 
      ctx->pid =(pid_t)(-1);
#endif
#else /*!HAVE_W32_SYSTEM*/
      CloseHandle ((HANDLE) ctx->pid);
      ctx->pid = (pid_t) INVALID_HANDLE_VALUE;
#endif /*HAVE_W32_SYSTEM*/
    }
  return 0;
}


static void
do_deinit (assuan_context_t ctx)
{
  do_finish (ctx);
}


/* Helper for pipe_connect. */
static gpg_error_t
initial_handshake (assuan_context_t *ctx)
{
  int okay, off;
  gpg_error_t err;
  
  err = _assuan_read_from_server (*ctx, &okay, &off);
  if (err)
    _assuan_log_printf ("can't connect server: %s\n",
                        gpg_strerror (err));
  else if (okay != 1)
    {
      _assuan_log_printf ("can't connect server: `%s'\n",
                          (*ctx)->inbound.line);
      err = _assuan_error (GPG_ERR_ASS_CONNECT_FAILED);
    }

  if (err)
    {
      assuan_disconnect (*ctx);
      *ctx = NULL;
    }
  return err;
}


#ifndef HAVE_W32_SYSTEM
#define pipe_connect pipe_connect_unix
/* Unix version of the pipe connection code.  We use an extra macro to
   make ChangeLog entries easier. */
static gpg_error_t
pipe_connect_unix (assuan_context_t *ctx,
                   const char *name, const char *const argv[],
                   int *fd_child_list,
                   void (*atfork) (void *opaque, int reserved),
                   void *atforkvalue, unsigned int flags)
{
  gpg_error_t err;
  int rp[2];
  int wp[2];
  char mypidstr[50];

  (void)flags;

  if (!ctx || !name || !argv || !argv[0])
    return _assuan_error (GPG_ERR_ASS_INV_VALUE);

  fix_signals ();

  sprintf (mypidstr, "%lu", (unsigned long)getpid ());

  if (pipe (rp) < 0)
    return _assuan_error (GPG_ERR_ASS_GENERAL);
  
  if (pipe (wp) < 0)
    {
      close (rp[0]);
      close (rp[1]);
      return _assuan_error (GPG_ERR_ASS_GENERAL);
    }

  err = _assuan_new_context (ctx);
  if (err)
    {
      close (rp[0]);
      close (rp[1]);
      close (wp[0]);
      close (wp[1]);
      return err;
    }
  (*ctx)->pipe_mode = 1;
  (*ctx)->inbound.fd  = rp[0];  /* Our inbound is read end of read pipe. */
  (*ctx)->outbound.fd = wp[1];  /* Our outbound is write end of write pipe. */
  (*ctx)->deinit_handler = do_deinit;
  (*ctx)->finish_handler = do_finish;

  /* FIXME: For GPGME we should better use _gpgme_io_spawn.  The PID
     stored here is actually soon useless.  */
  (*ctx)->pid = fork ();
  if ((*ctx)->pid < 0)
    {
      close (rp[0]);
      close (rp[1]);
      close (wp[0]);
      close (wp[1]);
      _assuan_release_context (*ctx); 
      return _assuan_error (GPG_ERR_ASS_GENERAL);
    }

  if ((*ctx)->pid == 0)
    {
#ifdef _ASSUAN_USE_DOUBLE_FORK      
      pid_t pid;

      if ((pid = fork ()) == 0)
#endif
	{
          int i, n;
          char errbuf[512];
          int *fdp;
          
          if (atfork)
            atfork (atforkvalue, 0);

          /* Dup handles to stdin/stdout. */
          if (rp[1] != STDOUT_FILENO)
            {
              if (dup2 (rp[1], STDOUT_FILENO) == -1)
                {
                  _assuan_log_printf ("dup2 failed in child: %s\n",
                                      strerror (errno));
                  _exit (4);
                }
            }
          if (wp[0] != STDIN_FILENO)
            {
              if (dup2 (wp[0], STDIN_FILENO) == -1)
                {
                  _assuan_log_printf ("dup2 failed in child: %s\n",
                                      strerror (errno));
                  _exit (4);
                }
            }

          /* Dup stderr to /dev/null unless it is in the list of FDs to be
             passed to the child. */
          fdp = fd_child_list;
          if (fdp)
            {
              for (; *fdp != -1 && *fdp != STDERR_FILENO; fdp++)
                ;
            }
          if (!fdp || *fdp == -1)
            {
              int fd = open ("/dev/null", O_WRONLY);
              if (fd == -1)
                {
                  _assuan_log_printf ("can't open `/dev/null': %s\n",
                                      strerror (errno));
                  _exit (4);
                }
              if (dup2 (fd, STDERR_FILENO) == -1)
                {
                  _assuan_log_printf ("dup2(dev/null, 2) failed: %s\n",
                                      strerror (errno));
                  _exit (4);
                }
            }


          /* Close all files which will not be duped and are not in the
             fd_child_list. */
          n = sysconf (_SC_OPEN_MAX);
          if (n < 0)
            n = MAX_OPEN_FDS;
          for (i=0; i < n; i++)
            {
              if ( i == STDIN_FILENO || i == STDOUT_FILENO
                   || i == STDERR_FILENO)
                continue;
              fdp = fd_child_list;
              if (fdp)
                {
                  while (*fdp != -1 && *fdp != i)
                    fdp++;
                }

              if (!(fdp && *fdp != -1))
                close(i);
            }
          errno = 0;

          /* We store our parents pid in the environment so that the
             execed assuan server is able to read the actual pid of the
             client.  The server can't use getppid because it might have
             been double forked before the assuan server has been
             initialized. */
          setenv ("_assuan_pipe_connect_pid", mypidstr, 1);

          /* Make sure that we never pass a connection fd variable
             when using a simple pipe.  */
          unsetenv ("_assuan_connection_fd");

          execv (name, (char *const *) argv); 
          /* oops - use the pipe to tell the parent about it */
          snprintf (errbuf, sizeof(errbuf)-1,
                    "ERR %d can't exec `%s': %.50s\n",
                    _assuan_error (GPG_ERR_ASS_SERVER_START),
                    name, strerror (errno));
          errbuf[sizeof(errbuf)-1] = 0;
          writen (1, errbuf, strlen (errbuf));
          _exit (4);
        }
#ifdef _ASSUAN_USE_DOUBLE_FORK
      if (pid == -1)
	_exit (1);
      else
	_exit (0);
#endif
    }

#ifdef _ASSUAN_USE_DOUBLE_FORK
  _assuan_waitpid ((*ctx)->pid, NULL, 0);
  (*ctx)->pid = -1;
#endif

  close (rp[1]);
  close (wp[0]);

  return initial_handshake (ctx);
}
#endif /*!HAVE_W32_SYSTEM*/


#ifndef HAVE_W32_SYSTEM
/* This function is similar to pipe_connect but uses a socketpair and
   sets the I/O up to use sendmsg/recvmsg. */
static gpg_error_t
socketpair_connect (assuan_context_t *ctx,
                    const char *name, const char *const argv[],
                    int *fd_child_list,
                    void (*atfork) (void *opaque, int reserved),
                    void *atforkvalue)
{
  gpg_error_t err;
  int fds[2];
  char mypidstr[50];

  if (!ctx
      || (name && (!argv || !argv[0]))
      || (!name && argv))
    return _assuan_error (GPG_ERR_ASS_INV_VALUE);

  fix_signals ();

  sprintf (mypidstr, "%lu", (unsigned long)getpid ());

  if ( socketpair (AF_LOCAL, SOCK_STREAM, 0, fds) )
    {
      _assuan_log_printf ("socketpair failed: %s\n", strerror (errno));
      return _assuan_error (GPG_ERR_ASS_GENERAL);
    }
  
  err = _assuan_new_context (ctx);
  if (err)
    {
      close (fds[0]);
      close (fds[1]);
      return err;
    }
  (*ctx)->pipe_mode = 1;
  (*ctx)->inbound.fd  = fds[0]; 
  (*ctx)->outbound.fd = fds[0]; 
  _assuan_init_uds_io (*ctx);
  (*ctx)->deinit_handler = _assuan_uds_deinit;
  (*ctx)->finish_handler = do_finish;

  (*ctx)->pid = fork ();
  if ((*ctx)->pid < 0)
    {
      close (fds[0]);
      close (fds[1]);
      _assuan_release_context (*ctx); 
      *ctx = NULL;
      return _assuan_error (GPG_ERR_ASS_GENERAL);
    }

  if ((*ctx)->pid == 0)
    {
#ifdef _ASSUAN_USE_DOUBLE_FORK      
      pid_t pid;

      if ((pid = fork ()) == 0)
#endif
	{
          int fd, i, n;
          char errbuf[512];
          int *fdp;
          
          if (atfork)
            atfork (atforkvalue, 0);

          /* Connect stdin and stdout to /dev/null. */
          fd = open ("/dev/null", O_RDONLY);
          if (fd == -1 || dup2 (fd, STDIN_FILENO) == -1)
            {
              _assuan_log_printf ("dup2(dev/null) failed: %s\n",
                                  strerror (errno));
              _exit (4);
            }
          fd = open ("/dev/null", O_WRONLY);
          if (fd == -1 || dup2 (fd, STDOUT_FILENO) == -1)
            {
              _assuan_log_printf ("dup2(dev/null) failed: %s\n",
                                  strerror (errno));
              _exit (4);
            }

          /* Dup stderr to /dev/null unless it is in the list of FDs to be
             passed to the child. */
          fdp = fd_child_list;
          if (fdp)
            {
              for (; *fdp != -1 && *fdp != STDERR_FILENO; fdp++)
                ;
            }
          if (!fdp || *fdp == -1)
            {
              fd = open ("/dev/null", O_WRONLY);
              if (fd == -1 || dup2 (fd, STDERR_FILENO) == -1)
                {
                  _assuan_log_printf ("dup2(dev/null) failed: %s\n",
                                      strerror (errno));
                  _exit (4);
                }
            }


          /* Close all files which will not be duped, are not in the
             fd_child_list and are not the connection fd. */
          n = sysconf (_SC_OPEN_MAX);
          if (n < 0)
            n = MAX_OPEN_FDS;
          for (i=0; i < n; i++)
            {
              if ( i == STDIN_FILENO || i == STDOUT_FILENO
                   || i == STDERR_FILENO || i == fds[1])
                continue;
              fdp = fd_child_list;
              if (fdp)
                {
                  while (*fdp != -1 && *fdp != i)
                    fdp++;
                }

              if (!(fdp && *fdp != -1))
                close(i);
            }
          errno = 0;

          /* We store our parents pid in the environment so that the
             execed assuan server is able to read the actual pid of the
             client.  The server can't use getppid becuase it might have
             been double forked before the assuan server has been
             initialized. */
          setenv ("_assuan_pipe_connect_pid", mypidstr, 1);

          /* Now set the environment variable used to convey the
             connection's file descriptor. */
          sprintf (mypidstr, "%d", fds[1]);
          if (setenv ("_assuan_connection_fd", mypidstr, 1))
            {
              _assuan_log_printf ("setenv failed: %s\n", strerror (errno));
              _exit (4);
            }

          if (!name && !argv)
            {
              /* No name and no args given, thus we don't do an exec
                 but continue the forked process.  */
              _assuan_release_context (*ctx);
              *ctx = NULL;
              return 0;
            }

          execv (name, (char *const *) argv); 
          /* oops - use the pipe to tell the parent about it */
          snprintf (errbuf, sizeof(errbuf)-1,
                    "ERR %d can't exec `%s': %.50s\n",
                    _assuan_error (GPG_ERR_ASS_SERVER_START),
                    name, strerror (errno));
          errbuf[sizeof(errbuf)-1] = 0;
          writen (fds[1], errbuf, strlen (errbuf));
          _exit (4);
        }
#ifdef _ASSUAN_USE_DOUBLE_FORK
      if (pid == -1)
	_exit (1);
      else
	_exit (0);
#endif
    }


#ifdef _ASSUAN_USE_DOUBLE_FORK
  _assuan_waitpid ((*ctx)->pid, NULL, 0);
  (*ctx)->pid = -1;
#endif

  close (fds[1]);
  
  return initial_handshake (ctx);
}
#endif /*!HAVE_W32_SYSTEM*/



#ifdef HAVE_W32_SYSTEM
/* Build a command line for use with W32's CreateProcess.  On success
   CMDLINE gets the address of a newly allocated string.  */
static int
build_w32_commandline (const char * const *argv, char **cmdline)
{
  int i, n;
  const char *s;
  char *buf, *p;

  *cmdline = NULL;
  n = 0;
  for (i=0; (s=argv[i]); i++)
    {
      n += strlen (s) + 1 + 2;  /* (1 space, 2 quoting */
      for (; *s; s++)
        if (*s == '\"')
          n++;  /* Need to double inner quotes.  */
    }
  n++;

  buf = p = _assuan_malloc (n);
  if (!buf)
    return -1;

  for (i=0; argv[i]; i++) 
    {
      if (i)
        p = stpcpy (p, " ");
      if (!*argv[i]) /* Empty string. */
        p = stpcpy (p, "\"\"");
      else if (strpbrk (argv[i], " \t\n\v\f\""))
        {
          p = stpcpy (p, "\"");
          for (s=argv[i]; *s; s++)
            {
              *p++ = *s;
              if (*s == '\"')
                *p++ = *s;
            }
          *p++ = '\"';
          *p = 0;
        }
      else
        p = stpcpy (p, argv[i]);
    }

  *cmdline= buf;
  return 0;
}
#endif /*HAVE_W32_SYSTEM*/


#ifdef HAVE_W32_SYSTEM
/* Create pipe where one end end is inheritable.  */
static int
create_inheritable_pipe (assuan_fd_t filedes[2], int for_write)
{
  HANDLE r, w, h;
  SECURITY_ATTRIBUTES sec_attr;

  memset (&sec_attr, 0, sizeof sec_attr );
  sec_attr.nLength = sizeof sec_attr;
  sec_attr.bInheritHandle = FALSE;
    
  if (!CreatePipe (&r, &w, &sec_attr, 0))
    {
      _assuan_log_printf ("CreatePipe failed: %s\n", w32_strerror (-1));
      return -1;
    }

  if (!DuplicateHandle (GetCurrentProcess(), for_write? r : w,
                        GetCurrentProcess(), &h, 0,
                        TRUE, DUPLICATE_SAME_ACCESS ))
    {
      _assuan_log_printf ("DuplicateHandle failed: %s\n", w32_strerror (-1));
      CloseHandle (r);
      CloseHandle (w);
      return -1;
    }
  if (for_write)
    {
      CloseHandle (r);
      r = h;
    }
  else
    {
      CloseHandle (w);
      w = h;
    }

  filedes[0] = r;
  filedes[1] = w;
  return 0;
}
#endif /*HAVE_W32_SYSTEM*/


#ifdef HAVE_W32_SYSTEM
#define pipe_connect pipe_connect_w32
/* W32 version of the pipe connection code. */
static gpg_error_t
pipe_connect_w32 (assuan_context_t *ctx,
                  const char *name, const char *const argv[],
                  int *fd_child_list,
                  void (*atfork) (void *opaque, int reserved),
                  void *atforkvalue, unsigned int flags)
{
  gpg_error_t err;
  assuan_fd_t rp[2];
  assuan_fd_t wp[2];
  char mypidstr[50];
  char *cmdline;
  SECURITY_ATTRIBUTES sec_attr;
  PROCESS_INFORMATION pi = 
    {
      NULL,      /* Returns process handle.  */
      0,         /* Returns primary thread handle.  */
      0,         /* Returns pid.  */
      0          /* Returns tid.  */
    };
  STARTUPINFO si;
  int fd, *fdp;
  HANDLE nullfd = INVALID_HANDLE_VALUE;

  if (!ctx || !name || !argv || !argv[0])
    return _assuan_error (GPG_ERR_ASS_INV_VALUE);

  fix_signals ();

  sprintf (mypidstr, "%lu", (unsigned long)getpid ());

  /* Build the command line.  */
  if (build_w32_commandline (argv, &cmdline))
    return _assuan_error (gpg_err_code from_syserror ());

  /* Create thew two pipes. */
  if (create_inheritable_pipe (rp, 0))
    {
      _assuan_free (cmdline);
      return _assuan_error (GPG_ERR_ASS_GENERAL);
    }
  
  if (create_inheritable_pipe (wp, 1))
    {
      CloseHandle (rp[0]);
      CloseHandle (rp[1]);
      _assuan_free (cmdline);
      return _assuan_error (GPG_ERR_ASS_GENERAL);
    }

  
  err = _assuan_new_context (ctx);
  if (err)
    {
      CloseHandle (rp[0]);
      CloseHandle (rp[1]);
      CloseHandle (wp[0]);
      CloseHandle (wp[1]);
      _assuan_free (cmdline);
      return _assuan_error (GPG_ERR_ASS_GENERAL);
    }

  (*ctx)->pipe_mode = 1;
  (*ctx)->inbound.fd  = rp[0];  /* Our inbound is read end of read pipe. */
  (*ctx)->outbound.fd = wp[1];  /* Our outbound is write end of write pipe. */
  (*ctx)->deinit_handler = do_deinit;
  (*ctx)->finish_handler = do_finish;


  /* fixme: Actually we should set the "_assuan_pipe_connect_pid" env
     variable.  However this requires us to write a full environment
     handler, because the strings are expected in sorted order.  The
     suggestion given in the MS Reference Library, to save the old
     value, changeit, create proces and restore it, is not thread
     safe.  */

  /* Start the process.  */
  memset (&sec_attr, 0, sizeof sec_attr );
  sec_attr.nLength = sizeof sec_attr;
  sec_attr.bInheritHandle = FALSE;
  
  memset (&si, 0, sizeof si);
  si.cb = sizeof (si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput  = wp[0];
  si.hStdOutput = rp[1];

  /* Dup stderr to /dev/null unless it is in the list of FDs to be
     passed to the child. */
  fd = fileno (stderr);
  fdp = fd_child_list;
  if (fdp)
    {
      for (; *fdp != -1 && *fdp != fd; fdp++)
        ;
    }
  if (!fdp || *fdp == -1)
    {
      nullfd = CreateFile ("nul", GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
      if (nullfd == INVALID_HANDLE_VALUE)
        {
          _assuan_log_printf ("can't open `nul': %s\n", w32_strerror (-1));
          CloseHandle (rp[0]);
          CloseHandle (rp[1]);
          CloseHandle (wp[0]);
          CloseHandle (wp[1]);
          _assuan_free (cmdline);
          _assuan_release_context (*ctx); 
          return -1;
        }
      si.hStdError = nullfd;
    }
  else
    si.hStdError = (void*)_get_osfhandle (fd);


  /* Note: We inherit all handles flagged as inheritable.  This seems
     to be a security flaw but there seems to be no way of selecting
     handles to inherit. */
  /*   _assuan_log_printf ("CreateProcess, path=`%s' cmdline=`%s'\n", */
  /*                       name, cmdline); */
  if (!CreateProcess (name,                 /* Program to start.  */
                      cmdline,              /* Command line arguments.  */
                      &sec_attr,            /* Process security attributes.  */
                      &sec_attr,            /* Thread security attributes.  */
                      TRUE,                 /* Inherit handles.  */
                      (CREATE_DEFAULT_ERROR_MODE
                       | ((flags & 128)? DETACHED_PROCESS : 0)
                       | GetPriorityClass (GetCurrentProcess ())
                       | CREATE_SUSPENDED), /* Creation flags.  */
                      NULL,                 /* Environment.  */
                      NULL,                 /* Use current drive/directory.  */
                      &si,                  /* Startup information. */
                      &pi                   /* Returns process information.  */
                      ))
    {
      _assuan_log_printf ("CreateProcess failed: %s\n", w32_strerror (-1));
      CloseHandle (rp[0]);
      CloseHandle (rp[1]);
      CloseHandle (wp[0]);
      CloseHandle (wp[1]);
      if (nullfd != INVALID_HANDLE_VALUE)
        CloseHandle (nullfd);
      _assuan_free (cmdline);
      _assuan_release_context (*ctx); 
      return _assuan_error (GPG_ERR_ASS_GENERAL);
    }
  _assuan_free (cmdline);
  cmdline = NULL;
  if (nullfd != INVALID_HANDLE_VALUE)
    {
      CloseHandle (nullfd);
      nullfd = INVALID_HANDLE_VALUE;
    }

  CloseHandle (rp[1]);
  CloseHandle (wp[0]);

  /*   _assuan_log_printf ("CreateProcess ready: hProcess=%p hThread=%p" */
  /*                       " dwProcessID=%d dwThreadId=%d\n", */
  /*                       pi.hProcess, pi.hThread, */
  /*                       (int) pi.dwProcessId, (int) pi.dwThreadId); */

  ResumeThread (pi.hThread);
  CloseHandle (pi.hThread); 
  (*ctx)->pid = (pid_t) pi.hProcess;

  return initial_handshake (ctx);
}
#endif /*HAVE_W32_SYSTEM*/


/* Connect to a server over a pipe, creating the assuan context and
   returning it in CTX.  The server filename is NAME, the argument
   vector in ARGV.  FD_CHILD_LIST is a -1 terminated list of file
   descriptors not to close in the child.  */
gpg_error_t
assuan_pipe_connect (assuan_context_t *ctx, const char *name,
		     const char *const argv[], int *fd_child_list)
{
  return pipe_connect (ctx, name, argv, fd_child_list, NULL, NULL, 0);
}


/* Connect to a server over a full-duplex socket (i.e. created by
   socketpair), creating the assuan context and returning it in CTX.
   The server filename is NAME, the argument vector in ARGV.
   FD_CHILD_LIST is a -1 terminated list of file descriptors not to
   close in the child.  ATFORK is called in the child right after the
   fork; ATFORKVALUE is passed as the first argument and 0 is passed
   as the second argument. The ATFORK function should only act if the
   second value is 0.

   FLAGS is a bit vector and controls how the function acts:
   Bit 0: If cleared a simple pipe based server is expected and the
          function behaves similar to `assuan_pipe_connect'.

          If set a server based on full-duplex pipes is expected. Such
          pipes are usually created using the `socketpair' function.
          It also enables features only available with such servers.

   Bit 7: If set and there is a need to start ther server it will be
          started as a background process.  This flag is useful under
          W32 systems, so that no new console is created and pops up a
          console window when starting the server


   If NAME as well as ARGV are NULL, no exec is done but the same
   process is continued.  However all file descriptors are closed and
   some special environment variables are set. To let the caller
   detect whether the child or the parent continues, the child returns
   a CTX of NULL. */
gpg_error_t
assuan_pipe_connect_ext (assuan_context_t *ctx,
                         const char *name, const char *const argv[],
                         int *fd_child_list,
                         void (*atfork) (void *opaque, int reserved),
                         void *atforkvalue, unsigned int flags)
{
  if ((flags & 1))
    {
#ifdef HAVE_W32_SYSTEM
      return _assuan_error (GPG_ERR_NOT_IMPLEMENTED);
#else
      return socketpair_connect (ctx, name, argv, fd_child_list,
                                 atfork, atforkvalue);
#endif
    }
  else
    return pipe_connect (ctx, name, argv, fd_child_list, atfork, atforkvalue,
                         flags);
}

