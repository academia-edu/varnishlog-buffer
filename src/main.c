#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <locale.h>

#define GLIB_VERSION_MIN_REQUIRED GLIB_VERSION_2_32
#include <glib.h>

#include "common.h"
#include "glib_extra.h"
#include "errors.h"
#include "die.h"
#include "varnishlog.h"
#include "priority.h"
#include "strings.h"

// Priority is arbitrary chosen, but should be lower than varnishlog's
#define HIGH_THREAD_PRIORITY 9

#define SENDER_SLEEP_NS (50*1000)

static volatile gint shutdown = false;

typedef struct WarnOptions {
	FILE *out;
	bool close_out;
	guint queue_size;
	time_t frequency_sec;
	struct timespec last_warn;
} WarnOptions;
G_STATIC_ASSERT(sizeof(time_t) >= sizeof(gint));

typedef struct SenderControl {
	GThread *thread;
	GSList *lines;
	volatile gint shutdown;
	WarnOptions *warnopt;
} SenderControl;

static void shutdown_sigaction() {
	// Ignore SIGPIPE. The return codes of writes will be checked.
	// stdout may have just gone away. We will still try to write any remaining
	// buffer, but if stdout is gone that will cause a SIGPIPE. We'll exit
	// gracefully if that happens rather than segfault.
	signal(SIGPIPE, SIG_IGN);
	g_atomic_int_set(&shutdown, true);
}

static bool register_signal_handlers( GError **err ) {
	struct sigaction act;
	memset(&act, 0, sizeof(act));
	act.sa_handler = (void (*)( int )) shutdown_sigaction;
	sigemptyset(&act.sa_mask);
	sigaddset(&act.sa_mask, SIGPIPE);

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

static void print_log_entry( GString *line, GError **err ) {
	g_assert(line != NULL);
	if( err != NULL && *err != NULL ) return;

	if( printf("%s\n", line->str) < 0 )
		g_set_error_errno(err);
}

static void string_free( GString *str ) {
	g_string_free(str, true);
}

static bool warn_if_too_long( GSList *lines, WarnOptions *warnopt, GError **error ) {
	guint lines_len = g_slist_length(lines);
	if( lines_len > warnopt->queue_size ) {
		struct timespec now;
		if( clock_gettime(CLOCK_MONOTONIC, &now) == -1 ) {
			g_set_error_errno(error);
			return false;
		}
		if( now.tv_sec - warnopt->last_warn.tv_sec > warnopt->frequency_sec ) {
			if( fprintf(warnopt->out, "Queue length too large (%u > %u)\n", lines_len, warnopt->queue_size) < 0 ) {
				g_set_error_errno(error);
				return false;
			}
			warnopt->last_warn = now;
		}
	}
	return true;
}

static GError *sender_main( SenderControl *control ) {
	GError *err = NULL;

	while( true ) {
		GSList *lines = (GSList *) g_atomic_pointer_and(&control->lines, 0);
		lines = g_slist_reverse(lines);

		if( !warn_if_too_long(lines, control->warnopt, &err) ) goto out_warn_if_too_long;

		g_slist_foreach(lines, (GFunc) print_log_entry, &err);
		if( err != NULL ) goto out_print_log_entry;
		g_slist_free_full(lines, (GDestroyNotify) string_free);

		if( g_atomic_int_get(&control->shutdown) && g_atomic_pointer_get(&control->lines) == NULL )
			break;

		usleep(SENDER_SLEEP_NS);

		continue;

out_warn_if_too_long:
		g_slist_foreach(lines, (GFunc) print_log_entry, NULL);
out_print_log_entry:
		g_slist_free_full(lines, (GDestroyNotify) string_free);

		goto out_loop_error;
	}

	return NULL;

out_loop_error:
	return err;
}

bool reader_and_writer_main( WarnOptions *warnopt, GError **err ) {
	Varnishlog *v = start_varnishlog(err);
	if( v == NULL ) goto out_start_varnishlog;
	if( !register_signal_handlers(err) ) goto out_register_signal_handlers;

	SenderControl sender_control = {
		.lines = NULL,
		.shutdown = false,
		.warnopt = warnopt
	};
	// Note that sender_control.thread might not be initialized when
	// the thread starts.
	sender_control.thread = g_thread_new("Rails Sender", (GThreadFunc) sender_main, &sender_control);

	if( !high_priority_thread(HIGH_THREAD_PRIORITY, err) ) goto out_high_priority_thread;

	while( !g_atomic_int_get(&shutdown) ) {
		GError *_err = NULL;
		GString *line = read_varnishlog_entry(v, &_err);

		if( line != NULL ) {
			GSList *lines = (GSList *) g_atomic_pointer_and(&sender_control.lines, 0);
			lines = g_slist_prepend(lines, line);
			g_atomic_pointer_set(&sender_control.lines, lines);
		}

		if( _err != NULL ) {
			if( g_atomic_int_get(&shutdown) ) {
				g_error_free(_err);
				break;
			} else if( _err->domain == ERRNO_QUARK && _err->code == EINTR ) {
				// Retry if the syscall was interrupted.
				g_error_free(_err);
				continue;
			}
			g_propagate_error(err, _err);
			goto out_read_varnishlog_entry;
		}
	}

	// This is just in case the call to ignore SIGPIPE in the signal handler
	// failed as that call isn't checked.
	if( signal(SIGPIPE, SIG_IGN) == SIG_ERR ) {
		g_set_error_errno(err);
		goto out_signal_sigpipe;
	}

	g_atomic_int_set(&sender_control.shutdown, true);

	GError *_err = g_thread_join(sender_control.thread);
	if( _err != NULL ) {
		g_propagate_error(err, _err);
		goto out_g_thread_join;
	}

	g_assert_cmpuint(g_slist_length(sender_control.lines), ==, 0);

	int stat;
	if( !shutdown_varnishlog(v, &stat, err) ) goto out_shutdown_varnishlog;

	if( !WIFSIGNALED(stat) || WTERMSIG(stat) != SIGINT )
		return stat;

	return true;

out_read_varnishlog_entry:
out_high_priority_thread:
out_signal_sigpipe:
	g_atomic_int_set(&sender_control.shutdown, true);
	g_thread_join(sender_control.thread);

	g_assert_cmpuint(g_slist_length(sender_control.lines), ==, 0);
out_g_thread_join:
out_shutdown_varnishlog:
out_register_signal_handlers:
	shutdown_varnishlog(v, NULL, NULL);
out_start_varnishlog:
	return false;
}

static bool set_stream_buffering( FILE *stream, int mode, GError **err ) {
	if( setvbuf(stream, NULL, mode, 0) != 0 ) {
		g_set_error_errno(err);
		return false;
	}
	return true;
}

gboolean set_buffer_mode( const gchar *option_name, const gchar *value, gpointer data, GError **err ) {
	(void) data, (void) option_name;

	int mode = _IOFBF;
	if(
		g_ascii_strcasecmp("unbuffered", value) == 0 ||
		g_ascii_strcasecmp("none", value) == 0
	) {
		mode = _IONBF;
	} else if( g_ascii_strcasecmp("line", value) == 0 ) {
		mode = _IOLBF;
	} else if(
		g_ascii_strcasecmp("block", value) == 0 ||
		g_ascii_strcasecmp("full", value) == 0
	) {
		mode = _IOFBF;
	} else {
		g_assert_not_reached();
	}

	return set_stream_buffering(stdout, mode, err);
}

int main( int argc, char *argv[] ) {
	setlocale(LC_ALL, "");

	GError *err = NULL;
	bool crash = true;

	gint warn_fd = 2;
	gint queue_size = 1000;
	gchar *warn_filename = NULL;
	WarnOptions warnopt;
	memset(&warnopt, 0, sizeof(warnopt));
	warnopt.frequency_sec = 5 * 60; // 5 minutes

	GOptionEntry option_entries[] = {
		{
			.long_name = "buffer-mode",
			.short_name = 'b',
			.flags = 0,
			.arg = G_OPTION_ARG_CALLBACK,
			.arg_data = set_buffer_mode,
			.description = "Set the output buffering mode",
			.arg_description = "(unbuffered|line|block)"
		},
		{ "queue-warn-size", 's', 0, G_OPTION_ARG_INT, &queue_size, "Warn if queue grows above this size", "size" },
		{ "warn-fd", 'w', 0, G_OPTION_ARG_INT, &warn_fd, "Write warnings to fd", "fd" },
		{ "warn-file", 'o', 0, G_OPTION_ARG_FILENAME, &warn_filename, "Write warnings to file", "file" },
		{ "warn-frequency-sec", 'f', 0, G_OPTION_ARG_INT, &warnopt.frequency_sec, "Write warning every N seconds", "N" },
		{ NULL, 0, 0, 0, NULL, NULL, NULL }
	};

	GOptionContext *option_context = g_option_context_new("- retrieve and queue varnish logs");
	g_option_context_add_main_entries(option_context, option_entries, NULL);

	if( !g_option_context_parse(option_context, &argc, &argv, &err) ) {
		crash = false;
		goto out_option_error;
	}

	if( queue_size < 0 ) {
		g_set_error_literal(
			&err,
			VARNISHLOG_BUFFER_QUARK,
			VARNISHLOG_BUFFER_ERROR_QUEUE_SIZE,
			"Cannot have a negative queue warn size"
		);
		crash = false;
		goto out_queue_size_lt_0;
	}
	warnopt.queue_size = queue_size;

	if( warn_filename != NULL ) {
		warnopt.close_out = true;
		if( (warnopt.out = fopen(warn_filename, "a")) == NULL ) {
			g_set_error_errno(&err);
			goto out_fopen;
		}
	} else {
		if( warn_fd == 1 ) {
			warnopt.out = stdout;
			warnopt.close_out = false;
		} else if( warn_fd == 2 ) {
			warnopt.out = stderr;
			warnopt.close_out = false;
		} else {
			warnopt.close_out = true;
			if( (warnopt.out = fdopen(warn_fd, "a")) == NULL ) {
				g_set_error_errno(&err);
				goto out_fdopen;
			}
		}
	}

	if( !set_stream_buffering(warnopt.out, _IOLBF, &err) ) goto out_set_stream_buffering;

	if( !reader_and_writer_main(&warnopt, &err) ) goto out_reader_and_writer_main;

	if( warnopt.close_out ) {
		if( fclose(warnopt.out) == EOF ) {
			g_set_error_errno(&err);
			goto out_fclose;
		}
	}

	if( warn_filename != NULL ) g_free(warn_filename);
	g_option_context_free(option_context);

	return EXIT_SUCCESS;

out_fclose:
out_reader_and_writer_main:
out_set_stream_buffering:
	if( warnopt.close_out ) fclose(warnopt.out);
out_fdopen:
out_fopen:
out_queue_size_lt_0:
out_option_error:
	if( warn_filename != NULL ) g_free(warn_filename);
	g_option_context_free(option_context);

	if( crash ) {
		g_die(err);
	} else {
		fprintf(stderr, "%s\n", err->message);
		return EXIT_FAILURE;
	}
}
