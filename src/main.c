#define _GNU_SOURCE
#define _ISOC11_SOURCE

#include <sched.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/wait.h>
#include <pthread.h>

#include <glib.h>

#define SHUTDOWN_POLL_TIME_MS 50

static struct {
	pid_t *pid;
	FILE *stdout;
} varnishlog;

static volatile bool shutdown = false;

typedef struct String {
	char *bytes;
	size_t len;
} String;

typedef struct SenderControl {
	GThread *thread;
	GSList *lines;
	GMutex lines_mutex;
	GCond lines_cond;
	volatile bool shutdown;
} SenderControl;

typedef enum AcademiaVarnishLogError {
	ACADEMIA_VARNISHLOG_ERROR_EOF,
	ACADEMIA_VARNISHLOG_ERROR_UNSPEC
} AcademiaVarnishLogError;

#define ACADEMIA_VARNISHLOG_QUARK academia_varnishlog_quark()
#define ACADEMIA_VARNISHLOG_ERRNO_QUARK academia_varnishlog_errno_quark()

static GQuark academia_varnishlog_quark() {
	return g_quark_from_static_string("academia-varnishlog-quark");
}

static GQuark academia_varnishlog_errno_quark() {
	return g_quark_from_static_string("academia-varnishlog-errno-quark");
}

static void g_set_error_errno( GError **err ) {
	int saved_errno = errno;
	g_set_error(
		err,
		ACADEMIA_VARNISHLOG_ERRNO_QUARK,
		saved_errno,
		"%s",
		strerror(saved_errno)
	);
}

__attribute__((noreturn))
static void _die_v( const char *fmt, va_list ap ) {
	fprintf(stderr, "%ju: ", (uintmax_t) getpid());
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
	g_assert(false); // Impossible
}

__attribute__((noreturn))
static void _die( const char *fmt, ... ) {
	va_list ap;
	va_start(ap, fmt);
	_die_v(fmt, ap);
	g_assert(false); // Impossible
}

#define dief(fmt, ...) (_die((fmt), ## __VA_ARGS__))

__attribute__((noreturn))
static void die( const char *msg ) {
	_die("%s", msg);
}

__attribute__((noreturn))
static void g_die( GError *err ) {
	if( err != NULL ) {
		die(err->message);
	} else {
		die("Unspecified error");
	}
}

static bool high_priority_process( int prio, GError **err ) {
#ifdef __linux__
	struct sched_param param = {0};
	param.sched_priority = prio;
	if( sched_setscheduler(getpid(), SCHED_FIFO, &param) == -1 ) {
		g_set_error_errno(err);
		return false;
	}
#else
#warning Cannot create SCHED_FIFO processes on non-linux platforms.
	(void) prio, (void) err;
#endif
	return true;
}

static bool high_priority_thread( int prio, GError **err ) {
	struct sched_param param;
	memset(&param, 0, sizeof(param));
	param.sched_priority = prio;
	// The type of the return value of pthread_setschedparam is actually undocumented,
	// so it may not be safe to assign it to errno.
	if( (errno = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param)) != 0 ) {
		g_set_error_errno(err);
		return false;
	}
	return true;
}

static bool shutdown_varnishlog( int *stat, GError **err ) {
	g_return_val_if_fail(varnishlog.pid != NULL, true);

	errno = 0;
	if( kill(*varnishlog.pid, SIGINT) == -1 ) {
		if( errno != ESRCH ) {
			g_set_error_errno(err);
			return false;
		}
	}
	if( fclose(varnishlog.stdout) != 0 ) {
		g_set_error_errno(err);
		return false;
	}
	varnishlog.stdout = NULL;
	if( waitpid(*varnishlog.pid, stat, 0) == -1 ) {
		g_set_error_errno(err);
		return false;
	}
	g_free(varnishlog.pid);
	varnishlog.pid = NULL;
	return true;
}

static void shutdown_sigaction() {
	shutdown = true;
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

static bool start_varnishlog( GError **err ) {
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

	varnishlog.pid = g_new(pid_t, 1);
	*varnishlog.pid = pid;
	if( (varnishlog.stdout = fdopen(pipes[0], "r")) == NULL ) {
		g_set_error_errno(err);
		goto out_fdopen;
	}
	if( close(pipes[1]) == -1 ) {
		g_set_error_errno(err);
		goto out_close_pipes_1;
	}

	return true;

out_close_pipes_1:
	fclose(varnishlog.stdout);
	varnishlog.stdout = NULL;
out_fdopen:
	kill(*varnishlog.pid, SIGINT);
	g_free(varnishlog.pid);
	varnishlog.pid = NULL;
out_fork:
	close(pipes[0]);
	close(pipes[1]);
out_pipe:
	return false;
}

static bool register_signal_handlers( GError **err ) {
	struct sigaction act;
	memset(&act, 0, sizeof(act));
	act.sa_handler = (void (*)( int )) shutdown_sigaction;
	//act.sa_flags = SA_RESTART;

	static const int shutdown_signals[] = {
		SIGHUP,
		SIGINT,
		SIGTERM
	};

	// Register signal handlers to kill varnishlog if we die.
	for( size_t i = 0; i < sizeof(shutdown_signals) / sizeof(int); i++ ) {
		struct sigaction oact;
		if( sigaction(shutdown_signals[i], &act, &oact) == -1 ) {
			// NOTE: Failure here doesn't reset signal handlers.
			g_set_error_errno(err);
			return false;
		}
	}

	return true;
}

static bool read_varnish_log_entry( char **line, size_t *len, GError **err ) {
	size_t allocation;
	ssize_t slen;
	errno = 0;
	do {
		slen = getline(line, &allocation, varnishlog.stdout);
		if( slen == -1 ) {
			if( errno != 0 ) {
				if( errno == EINTR && !shutdown )
					continue;
				g_set_error_errno(err);
			} else if( feof(varnishlog.stdout) ) {
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
	} while( slen == -1 );
	*len = slen;
	if( (*line)[slen - 1] == '\n' ) (*line)[slen - 1] = '\0';
	return true;
}

static void send_log_entry_to_rails( String *line ) {
	// TODO: Run this in a low priority process.
	printf("%s\n", line->bytes);
}

static void free_string( String *line ) {
	free(line->bytes);
	g_slice_free(String, line);
}

static void *rails_sender_main( SenderControl *control ) {
	while( !shutdown ) {
		g_mutex_lock(&control->lines_mutex);
		while( control->lines == NULL && !shutdown ) {
			gint64 end_time = g_get_monotonic_time() + SHUTDOWN_POLL_TIME_MS * G_TIME_SPAN_MILLISECOND;
			g_cond_wait_until(&control->lines_cond, &control->lines_mutex, end_time);
		}

		// Keep in mind lines may be NULL
		GSList *lines = control->lines;
		control->lines = NULL;
		g_mutex_unlock(&control->lines_mutex);

		lines = g_slist_reverse(lines);

		g_slist_foreach(lines, (GFunc) send_log_entry_to_rails, NULL);
		g_slist_free_full(lines, (GDestroyNotify) free_string);
	}
	return NULL;
}

int main() {
	GError *err = NULL;

	if( !start_varnishlog(&err) ) goto out_start_varnishlog;
	if( !register_signal_handlers(&err) ) goto out_register_signal_handlers;

	SenderControl sender_control = {
		.thread = g_thread_new("Rails Sender", (GThreadFunc) rails_sender_main, &sender_control),
		.lines = NULL
	};
	g_mutex_init(&sender_control.lines_mutex);
	g_cond_init(&sender_control.lines_cond);

	// Priority is arbitrary chosen, but should be lower than varnishlog's
	if( !high_priority_thread(9, &err) ) goto out_high_priority_thread;

	while( !shutdown ) {
		String *line = g_slice_new(String);
		memset(line, 0, sizeof(*line));

		if( !read_varnish_log_entry(&line->bytes, &line->len, &err) ) {
			if( shutdown ) {
				g_clear_error(&err);
				break;
			}
			goto out_read_varnish_log_entry;
		}

		g_mutex_lock(&sender_control.lines_mutex);
		sender_control.lines = g_slist_prepend(sender_control.lines, line);
		g_cond_signal(&sender_control.lines_cond);
		g_mutex_unlock(&sender_control.lines_mutex);
	}

	sender_control.shutdown = true;

	// Wake up the other thread
	g_cond_broadcast(&sender_control.lines_cond);

	g_thread_join(sender_control.thread);

	g_mutex_clear(&sender_control.lines_mutex);
	g_cond_clear(&sender_control.lines_cond);

	int stat;
	if( !shutdown_varnishlog(&stat, &err) ) goto out_shutdown_varnishlog;

	if( !WIFSIGNALED(stat) || WTERMSIG(stat) != SIGINT )
		return stat;

	return EXIT_SUCCESS;

out_shutdown_varnishlog:
out_read_varnish_log_entry:
out_high_priority_thread:
	sender_control.shutdown = true;

	g_mutex_lock(&sender_control.lines_mutex);
	if( sender_control.lines != NULL ) {
		g_slist_free_full(sender_control.lines, (GDestroyNotify) free_string);
		sender_control.lines = NULL;
	}
	g_cond_broadcast(&sender_control.lines_cond);
	g_mutex_unlock(&sender_control.lines_mutex);
out_register_signal_handlers:
	shutdown_varnishlog(NULL, NULL);
out_start_varnishlog:
	g_die(err);
}
