#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

#include <glib.h>

#include "common.h"
#include "glib_extra.h"
#include "varnishlog.h"
#include "die.h"
#include "priority.h"
#include "errors.h"

struct Varnishlog {
	pid_t *pid;
	FILE *stdout;
	GIOChannel *error_channel;
};

bool shutdown_varnishlog( Varnishlog *v, int *stat, GError **err ) {
	if( v->pid != NULL ) {
		errno = 0;
		if( kill(*v->pid, SIGINT) == -1 ) {
			if( errno != ESRCH ) {
				g_set_error_errno(err);
				return false;
			}
		}

		if( waitpid(*v->pid, stat, 0) == -1 ) {
			g_set_error_errno(err);
			return false;
		}

		g_free(v->pid);
		v->pid = NULL;
	}

	if( v->stdout != NULL ) {
		if( fclose(v->stdout) != 0 ) {
			g_set_error_errno(err);
			return false;
		}
		v->stdout = NULL;
	}

	if( v->error_channel != NULL ) {
		GIOStatus status = g_io_channel_shutdown(v->error_channel, false, err);
		g_assert(status != G_IO_STATUS_AGAIN);
		g_assert(status != G_IO_STATUS_EOF);
		if( status != G_IO_STATUS_NORMAL )
			return false;
		g_io_channel_unref(v->error_channel);
		v->error_channel = NULL;
	}

	g_slice_free(Varnishlog, v);

	return true;
}

__attribute__((noreturn))
static void start_varnishlog_child_noreturn( int pipes[2], int error_pipes[2], GIOChannel *error_out ) {
	GError *err = NULL;

	if( close(error_pipes[0]) == -1 ) goto out_close_error_pipes_0;
	if( close(pipes[0]) == -1 ) goto out_close_pipes_0;
	if( close(1) == -1 ) goto out_close_1;
	if( dup2(pipes[1], 1) == -1 ) goto out_dup2;
	if( close(pipes[1]) == -1 ) goto out_close_pipes_1;

	// The priority is arbitrarily chosen. Priorities range from 1 - 99. See chrt -m
	if( !high_priority_process(10, &err) ) goto out_high_priority_process;

	char *argv[] = {
		"varnishlog",
		"-cOu",
		NULL
	};

	execvp(argv[0], argv);
	// Fall through to error cases if we get here.

out_close_pipes_1:
out_dup2:
out_close_1:
out_close_pipes_0:
out_close_error_pipes_0:
	g_set_error_errno(&err);
out_high_priority_process:
	if( !write_gerror(error_out, err, NULL) )
		g_die(err);
	exit(EXIT_FAILURE);
	g_assert(false); // l'impossible!
}

static volatile bool child_error_waiting = false;
static int child_error_fd;

// Be careful, this function is called in a signal handler context.
static void child_error_io_ready() {
	int saved_errno = errno;
	errno = 0;
	if( fcntl(child_error_fd, F_GETFD) != -1 || errno != EBADF )
		g_atomic_int_set(&child_error_waiting, true);
	errno = saved_errno;
}

// Note that only one Varnishlog may exist at a time.
Varnishlog *start_varnishlog( GError **err ) {
	int pipes[2], error_pipes[2];
	bool closed_pipes_1 = false, closed_error_pipes_1 = false;

	if( pipe(pipes) == -1 ) {
		g_set_error_errno(err);
		goto out_pipes;
	}

	if( pipe(error_pipes) == -1 ) {
		g_set_error_errno(err);
		goto out_error_pipes;
	}

	child_error_fd = error_pipes[0];

	if( fcntl(error_pipes[0], F_SETFL, O_ASYNC) == -1 ) {
		g_set_error_errno(err);
		goto out_error_pipes_fcntl;
	}

	if( fcntl(error_pipes[0], F_SETOWN, getpid()) == -1 ) {
		g_set_error_errno(err);
		goto out_error_pipes_fcntl;
	}

	struct sigaction act, oact;
	memset(&act, 0, sizeof(act));
	act.sa_handler = (void (*)( int )) child_error_io_ready;
	if( sigaction(SIGIO, &act, &oact) == -1 ) {
		g_set_error_errno(err);
		goto out_error_pipes_sigaction;
	}

	GIOChannel *error_write = g_io_channel_unix_new(error_pipes[1]),
	           *error_read = g_io_channel_unix_new(error_pipes[0]);

	if( g_io_channel_set_encoding(error_write, NULL, err) != G_IO_STATUS_NORMAL )
		goto out_error_write_set_encoding;

	if( g_io_channel_set_encoding(error_read, NULL, err) != G_IO_STATUS_NORMAL )
		goto out_error_read_set_encoding;

	pid_t pid = fork();
	if( pid == -1 ) {
		g_set_error_errno(err);
		goto out_fork;
	} else if( pid == 0 ) {
		g_io_channel_unref(error_read);
		start_varnishlog_child_noreturn(pipes, error_pipes, error_write);
	}

	g_io_channel_unref(error_write);

	if( close(pipes[1]) == -1 ) {
		g_set_error_errno(err);
		goto out_close_pipes_1;
	}
	closed_pipes_1 = true;

	if( close(error_pipes[1]) == -1 ) {
		g_set_error_errno(err);
		goto out_close_error_pipes_1;
	}
	closed_error_pipes_1 = true;

	FILE *child_stdout;
	if( (child_stdout = fdopen(pipes[0], "r")) == NULL ) {
		g_set_error_errno(err);
		goto out_fdopen_pipes_0;
	}

	Varnishlog *v = g_slice_new(Varnishlog);
	v->pid = g_new(pid_t, 1);
	*v->pid = pid;
	v->error_channel = error_read;
	v->stdout = child_stdout;

	return v;

out_fdopen_pipes_0:
out_close_error_pipes_1:
out_close_pipes_1:
	kill(pid, SIGINT);
out_fork:
out_error_read_set_encoding:
out_error_write_set_encoding:
	g_io_channel_unref(error_read);
	g_io_channel_unref(error_write);
	sigaction(SIGIO, &oact, &act);
out_error_pipes_sigaction:
out_error_pipes_fcntl:
	close(error_pipes[0]);
	if( !closed_error_pipes_1 ) close(error_pipes[1]);
out_error_pipes:
	close(pipes[0]);
	if( !closed_pipes_1 ) close(pipes[1]);
out_pipes:
	return NULL;
}

static bool set_error_from_child_if_pending( Varnishlog *v, GError **err ) {
	if( !g_atomic_int_get(&child_error_waiting) ) return false;

	GError *cld_err = read_gerror(v->error_channel, err);
	if( cld_err == NULL ) return false;
	// Note that there is a race here if the child wants to send more errors.
	//                                Meh.
	g_atomic_int_set(&child_error_waiting, false);
	g_propagate_error(err, cld_err);
	return true;
}

GString *read_varnishlog_entry( Varnishlog *v, GError **err ) {
	size_t allocation;
	char *line = NULL;
	errno = 0;
	ssize_t slen = getline(&line, &allocation, v->stdout);
	int saved_errno = errno;
	if( slen == -1 ) {
		GError *_err = NULL;
		if( set_error_from_child_if_pending(v, &_err) ) {
			// Got an error from the child.
			g_propagate_error(err, _err);
		} else {
			// Didn't get an error from the child.
			if( _err == NULL ) {
				errno = saved_errno;
				set_gerror_getline(v->stdout, err);
			} else {
				g_propagate_error(err, _err);
			}
		}
		return NULL;
	}
	if( line[slen - 1] == '\n' ) line[slen - 1] = '\0';

	GString *ret = g_string_wrap(line, slen, allocation);

	set_error_from_child_if_pending(v, err);

	return ret;
}
