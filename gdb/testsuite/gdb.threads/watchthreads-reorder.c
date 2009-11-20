/* This testcase is part of GDB, the GNU debugger.

   Copyright 2009 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <signal.h>
#include <unistd.h>
#include <asm/unistd.h>

#define gettid() syscall (__NR_gettid)

/* Terminate always in the main task, it can lock up with SIGSTOPped GDB
   otherwise.  */
#define TIMEOUT (gettid () == getpid() ? 10 : 15)

static pthread_mutex_t gdbstop_mutex = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;

static pid_t thread1_tid;
static pthread_cond_t thread1_tid_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t thread1_tid_mutex = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;

static pid_t thread2_tid;
static pthread_cond_t thread2_tid_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t thread2_tid_mutex = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;

static pthread_mutex_t terminate_mutex = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;

/* These variables must have lower in-memory addresses than thread1_rwatch and
   thread2_rwatch so that they take their watchpoint slots.  */

static int unused1_rwatch;
static int unused2_rwatch;

static volatile int thread1_rwatch;
static volatile int thread2_rwatch;

/* Do not use alarm as it would create a ptrace event which would hang up us if
   we are being traced by GDB which we stopped ourselves.  */

static void timed_mutex_lock (pthread_mutex_t *mutex)
{
  int i;
  struct timespec start, now;

  i = clock_gettime (CLOCK_MONOTONIC, &start);
  assert (i == 0);

  do
    {
      i = pthread_mutex_trylock (mutex);
      if (i == 0)
	return;
      assert (i == EBUSY);

      i = clock_gettime (CLOCK_MONOTONIC, &now);
      assert (i == 0);
      assert (now.tv_sec >= start.tv_sec);
    }
  while (now.tv_sec - start.tv_sec < TIMEOUT);

  fprintf (stderr, "Timed out waiting for internal lock!\n");
  exit (EXIT_FAILURE);
}

static void *
thread1_func (void *unused)
{
  int i;
  volatile int rwatch_store;

  thread1_tid = gettid ();
  i = pthread_cond_signal (&thread1_tid_cond);
  assert (i == 0);

  /* Be sure GDB is already stopped before continuing.  */
  timed_mutex_lock (&gdbstop_mutex);
  i = pthread_mutex_unlock (&gdbstop_mutex);
  assert (i == 0);

  rwatch_store = thread1_rwatch;

  /* Be sure the "T (tracing stop)" test can proceed for both threads.  */
  timed_mutex_lock (&terminate_mutex);
  i = pthread_mutex_unlock (&terminate_mutex);
  assert (i == 0);

  return NULL;
}

static void *
thread2_func (void *unused)
{
  int i;
  volatile int rwatch_store;

  thread2_tid = gettid ();
  i = pthread_cond_signal (&thread2_tid_cond);
  assert (i == 0);

  /* Be sure GDB is already stopped before continuing.  */
  timed_mutex_lock (&gdbstop_mutex);
  i = pthread_mutex_unlock (&gdbstop_mutex);
  assert (i == 0);

  rwatch_store = thread2_rwatch;

  /* Be sure the "T (tracing stop)" test can proceed for both threads.  */
  timed_mutex_lock (&terminate_mutex);
  i = pthread_mutex_unlock (&terminate_mutex);
  assert (i == 0);

  return NULL;
}

static const char *
proc_string (const char *filename, const char *line)
{
  FILE *f;
  static char buf[LINE_MAX];
  size_t line_len = strlen (line);

  f = fopen (filename, "r");
  if (f == NULL)
    {
      fprintf (stderr, "fopen (\"%s\") for \"%s\": %s\n", filename, line,
	       strerror (errno));
      exit (EXIT_FAILURE);
    }
  while (errno = 0, fgets (buf, sizeof (buf), f))
    {
      char *s;

      s = strchr (buf, '\n');
      assert (s != NULL);
      *s = 0;

      if (strncmp (buf, line, line_len) != 0)
	continue;

      if (fclose (f))
	{
	  fprintf (stderr, "fclose (\"%s\") for \"%s\": %s\n", filename, line,
		   strerror (errno));
	  exit (EXIT_FAILURE);
	}

      return &buf[line_len];
    }
  if (errno != 0)
    {
      fprintf (stderr, "fgets (\"%s\": %s\n", filename, strerror (errno));
      exit (EXIT_FAILURE);
    }
  fprintf (stderr, "\"%s\": No line \"%s\" found.\n", filename, line);
  exit (EXIT_FAILURE);
}

static unsigned long
proc_ulong (const char *filename, const char *line)
{
  const char *s = proc_string (filename, line);
  long retval;
  char *end;

  errno = 0;
  retval = strtol (s, &end, 10);
  if (retval < 0 || retval >= LONG_MAX || (end && *end))
    {
      fprintf (stderr, "\"%s\":\"%s\": %ld, %s\n", filename, line, retval,
	       strerror (errno));
      exit (EXIT_FAILURE);
    }
  return retval;
}

static void
state_wait (pid_t process, const char *wanted)
{
  char *filename;
  int i;
  struct timespec start, now;
  const char *state;

  i = asprintf (&filename, "/proc/%lu/status", (unsigned long) process);
  assert (i > 0);

  i = clock_gettime (CLOCK_MONOTONIC, &start);
  assert (i == 0);

  do
    {
      state = proc_string (filename, "State:\t");
      if (strcmp (state, wanted) == 0)
	{
	  free (filename);
	  return;
	}

      if (sched_yield ())
	{
	  perror ("sched_yield()");
	  exit (EXIT_FAILURE);
	}

      i = clock_gettime (CLOCK_MONOTONIC, &now);
      assert (i == 0);
      assert (now.tv_sec >= start.tv_sec);
    }
  while (now.tv_sec - start.tv_sec < TIMEOUT);

  fprintf (stderr, "Timed out waiting for PID %lu \"%s\" (now it is \"%s\")!\n",
	   (unsigned long) process, wanted, state);
  exit (EXIT_FAILURE);
}

static volatile pid_t tracer = 0;
static pthread_t thread1, thread2;

static void
cleanup (void)
{
  printf ("Resuming GDB PID %lu.\n", (unsigned long) tracer);

  if (tracer)
    {
      int i;
      int tracer_save = tracer;

      tracer = 0;

      i = kill (tracer_save, SIGCONT);
      assert (i == 0);
    }
}

int
main (int argc, char **argv)
{
  int i;
  int standalone = 0;

  if (argc == 2 && strcmp (argv[1], "-s") == 0)
    standalone = 1;
  else
    assert (argc == 1);

  setbuf (stdout, NULL);

  timed_mutex_lock (&gdbstop_mutex);

  timed_mutex_lock (&terminate_mutex);

  i = pthread_create (&thread1, NULL, thread1_func, NULL);
  assert (i == 0);

  i = pthread_create (&thread2, NULL, thread2_func, NULL);
  assert (i == 0);

  if (!standalone)
    {
      tracer = proc_ulong ("/proc/self/status", "TracerPid:\t");
      if (tracer == 0)
	{
	  fprintf (stderr, "The testcase must be run by GDB!\n");
	  exit (EXIT_FAILURE);
	}
      if (tracer != getppid ())
	{
	  fprintf (stderr, "The testcase parent must be our GDB tracer!\n");
	  exit (EXIT_FAILURE);
	}
    }

  /* SIGCONT our debugger in the case of our crash as we would deadlock
     otherwise.  */

  atexit (cleanup);

  printf ("Stopping GDB PID %lu.\n", (unsigned long) tracer);

  if (tracer)
    {
      i = kill (tracer, SIGSTOP);
      assert (i == 0);
      state_wait (tracer, "T (stopped)");
    }

  timed_mutex_lock (&thread1_tid_mutex);
  timed_mutex_lock (&thread2_tid_mutex);

  /* Let the threads start.  */
  i = pthread_mutex_unlock (&gdbstop_mutex);
  assert (i == 0);

  printf ("Waiting till the threads initialize their TIDs.\n");

  if (thread1_tid == 0)
    {
      i = pthread_cond_wait (&thread1_tid_cond, &thread1_tid_mutex);
      assert (i == 0);

      assert (thread1_tid > 0);
    }

  if (thread2_tid == 0)
    {
      i = pthread_cond_wait (&thread2_tid_cond, &thread2_tid_mutex);
      assert (i == 0);

      assert (thread2_tid > 0);
    }

  printf ("Thread 1 TID = %lu, thread 2 TID = %lu, PID = %lu.\n",
	  (unsigned long) thread1_tid, (unsigned long) thread2_tid,
	  (unsigned long) getpid ());

  printf ("Waiting till the threads get trapped by the watchpoints.\n");

  if (tracer)
    {
      /* s390x-unknown-linux-gnu will fail with "R (running)".  */

      state_wait (thread1_tid, "T (tracing stop)");

      state_wait (thread2_tid, "T (tracing stop)");
    }

  cleanup ();

  printf ("Joining the threads.\n");

  i = pthread_mutex_unlock (&terminate_mutex);
  assert (i == 0);

  i = pthread_join (thread1, NULL);
  assert (i == 0);

  i = pthread_join (thread2, NULL);
  assert (i == 0);

  printf ("Exiting.\n");	/* break-at-exit */

  /* Just prevent compiler `warning: unusedX_rwatch defined but not used'.  */
  unused1_rwatch = 1;
  unused2_rwatch = 2;

  return EXIT_SUCCESS;
}
