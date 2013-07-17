#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <glib.h>

#include "common.h"
#include "glib_extra.h"
#include "varnishlog.h"
#include "die.h"
#include "priority.h"
#include "shutdown.h"
#include "errors.h"

struct Varnishlog {
	pid_t *pid;
	FILE *stdout;
};

bool shutdown_varnishlog( Varnishlog *v, int *stat, GError **err ) {
	g_return_val_if_fail(v->pid != NULL, true);

	errno = 0;
	if( kill(*v->pid, SIGINT) == -1 ) {
		if( errno != ESRCH ) {
			g_set_error_errno(err);
			return false;
		}
	}
	if( fclose(v->stdout) != 0 ) {
		g_set_error_errno(err);
		return false;
	}
	v->stdout = NULL;
	if( waitpid(*v->pid, stat, 0) == -1 ) {
		g_set_error_errno(err);
		return false;
	}
	g_free(v->pid);
	v->pid = NULL;
	return true;
}

__attribute__((noreturn))
static void start_varnishlog_child_noreturn( int pipes[2] ) {
	// TODO: Have a way to communicate a GError to the parent
	GError *err = NULL;

	if( close(pipes[0]) == -1 ) die(strerror(errno));
	if( close(1) == -1 ) die(strerror(errno));
	if( dup2(pipes[1], 1) == -1 ) die(strerror(errno));
	if( close(pipes[1]) == -1 ) die(strerror(errno));

	// The priority is arbitrarily chosen. Priorities range from 1 - 99. See chrt -m
	if( !high_priority_process(10, &err) ) g_die(err);

	char *argv[] = {
		"varnishlog",
		"-cOu",
		NULL
	};

	execvp(argv[0], argv);
	die(strerror(errno));
}

Varnishlog *start_varnishlog( GError **err ) {
	int pipes[2];
	if( pipe(pipes) == -1 ) {
		g_set_error_errno(err);
		goto out_pipe;
	}

	pid_t pid = fork();
	if( pid == -1 ) {
		g_set_error_errno(err);
		goto out_fork;
	} else if( pid == 0 ) {
		start_varnishlog_child_noreturn(pipes);
	}

	Varnishlog *v = g_slice_new(Varnishlog);
	v->pid = g_new(pid_t, 1);
	*v->pid = pid;
	if( (v->stdout = fdopen(pipes[0], "r")) == NULL ) {
		g_set_error_errno(err);
		goto out_fdopen;
	}
	if( close(pipes[1]) == -1 ) {
		g_set_error_errno(err);
		goto out_close_pipes_1;
	}

	return v;

out_close_pipes_1:
	fclose(v->stdout);
	v->stdout = NULL;
out_fdopen:
	kill(*v->pid, SIGINT);
	g_free(v->pid);
	v->pid = NULL;
	g_slice_free(Varnishlog, v);
out_fork:
	close(pipes[0]);
	close(pipes[1]);
out_pipe:
	return NULL;
}

bool read_varnishlog_entry( Varnishlog *v, char **line, size_t *len, GError **err ) {
	size_t allocation;
	errno = 0;
	ssize_t slen = getline(line, &allocation, v->stdout);
	if( slen == -1 ) {
		if( errno != 0 ) {
			g_set_error_errno(err);
		} else if( feof(v->stdout) ) {
			g_set_error_literal(
				err,
				ACADEMIA_VARNISHLOG_QUARK,
				ACADEMIA_VARNISHLOG_ERROR_EOF,
				"End of file found on varnishlog pipe"
			);
		} else {
			g_set_error_literal(
				err,
				ACADEMIA_VARNISHLOG_QUARK,
				ACADEMIA_VARNISHLOG_ERROR_UNSPEC,
				"Unspecified error reading varnishlog pipe"
			);
		}
		return false;
	}
	*len = slen;
	if( (*line)[slen - 1] == '\n' ) (*line)[slen - 1] = '\0';
	return true;
}
