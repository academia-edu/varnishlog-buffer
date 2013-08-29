#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <locale.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

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

typedef struct SenderControl {
	GThread *thread;
	GSList *lines;
	volatile gint shutdown;
	volatile gint *lines_len;
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

typedef struct PrintLogEntryContext {
	GError **error;
	volatile gint *lines_len;
} PrintLogEntryContext;

static void print_log_entry( GString *line, PrintLogEntryContext *ctx ) {
	g_assert(line != NULL);
	if( ctx->error != NULL && *ctx->error != NULL ) return;

	if( printf("%s\n", line->str) < 0 )
		g_set_error_errno(ctx->error);

	g_atomic_int_dec_and_test(ctx->lines_len);
}

static void string_free( GString *str ) {
	g_string_free(str, true);
}

static GError *sender_main( SenderControl *control ) {
	GError *err = NULL;

	PrintLogEntryContext plec = {
		.error = &err,
		.lines_len = control->lines_len
	};

	while( true ) {
		GSList *lines = (GSList *) g_atomic_pointer_and(&control->lines, 0);
		lines = g_slist_reverse(lines);

		g_slist_foreach(lines, (GFunc) print_log_entry, &plec);
		if( err != NULL ) goto out_print_log_entry;
		g_slist_free_full(lines, (GDestroyNotify) string_free);

		if( g_atomic_int_get(&control->shutdown) ) {
			if( g_atomic_pointer_get(&control->lines) == NULL ) {
				break;
			} else {
				continue;
			}
		}

		usleep(SENDER_SLEEP_NS);

		continue;

out_print_log_entry:
		g_slist_free_full(lines, (GDestroyNotify) string_free);

		goto out_loop_error;
	}

	return NULL;

out_loop_error:
	return err;
}

static gint *new_lines_len_ptr( int fd, GError **error ) {
	int mmap_flags = MAP_SHARED;
	if( fd == -1 ) mmap_flags |= MAP_ANON;
#ifdef __linux__
	mmap_flags |= MAP_LOCKED;
#endif
	gint *lines_len = mmap(NULL, sizeof(gint), PROT_READ | PROT_WRITE, mmap_flags, fd, 0);
	if( lines_len == MAP_FAILED ) {
		g_set_error_errno(error);
		return NULL;
	}
	return lines_len;
}

static bool free_lines_len_ptr( gint *lines_len, GError **error ) {
	if( munmap(lines_len, sizeof(gint)) == -1 ) {
		g_set_error_errno(error);
		return false;
	}
	return true;
}

static bool reader_and_writer_main( int lines_len_fd, gboolean lowprio, GError **err ) {
	Varnishlog *v = start_varnishlog(lowprio, err);
	if( v == NULL ) goto err_setup_start_varnishlog;
	if( !register_signal_handlers(err) ) goto err_setup_register_signal_handlers;

	volatile gint *lines_len = new_lines_len_ptr(lines_len_fd, err);
	if( lines_len == NULL ) goto err_setup_new_lines_len_ptr;

	SenderControl sender_control = {
		.lines = NULL,
		.shutdown = false,
		.lines_len = lines_len
	};
	// Note that sender_control.thread might not be initialized when
	// the thread starts.
	sender_control.thread = g_thread_new("Rails Sender", (GThreadFunc) sender_main, &sender_control);

	if( !lowprio && !high_priority_thread(HIGH_THREAD_PRIORITY, err) ) goto err_setup_high_priority_thread;

	while( !g_atomic_int_get(&shutdown) ) {
		GError *_err = NULL;
		GString *line = read_varnishlog_entry(v, &_err);

		if( line != NULL ) {
			GSList *lines = (GSList *) g_atomic_pointer_and(&sender_control.lines, 0);
			lines = g_slist_prepend(lines, line);

			// We'll probably run out of memory long before this is a problem, but just in case...
			g_assert_cmpint(g_atomic_int_get(lines_len), <, G_MAXINT);
			g_assert_cmpint(g_atomic_int_get(lines_len), >=, 0);
			g_atomic_int_inc(lines_len);

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
			goto err_read_varnishlog_entry;
		}
	}

	// This is just in case the call to ignore SIGPIPE in the signal handler
	// failed as that call isn't checked.
	if( signal(SIGPIPE, SIG_IGN) == SIG_ERR ) {
		g_set_error_errno(err);
		goto err_teardown_signal_sigpipe;
	}

	g_atomic_int_set(&sender_control.shutdown, true);

	GError *_err = g_thread_join(sender_control.thread);
	if( _err != NULL ) {
		g_propagate_error(err, _err);
		goto err_teardown_g_thread_join;
	}

	g_assert_cmpuint(g_slist_length(sender_control.lines), ==, 0);
	g_assert_cmpuint(g_atomic_int_get(lines_len), ==, 0);

	if( !free_lines_len_ptr((gint *) lines_len, err) ) goto err_teardown_free_lines_len_ptr;

	int stat;
	if( !shutdown_varnishlog(v, &stat, err) ) goto err_teardown_shutdown_varnishlog;

	if( !WIFSIGNALED(stat) || WTERMSIG(stat) != SIGINT )
		return stat;

	return true;

err_read_varnishlog_entry:
err_setup_high_priority_thread:
err_teardown_signal_sigpipe:
	g_atomic_int_set(&sender_control.shutdown, true);
	g_thread_join(sender_control.thread);

	g_assert_cmpuint(g_slist_length(sender_control.lines), ==, 0);
	g_assert_cmpuint(g_atomic_int_get(lines_len), ==, 0);
err_teardown_g_thread_join:
	free_lines_len_ptr((gint *) lines_len, NULL);
err_teardown_free_lines_len_ptr:
err_setup_new_lines_len_ptr:
err_setup_register_signal_handlers:
	shutdown_varnishlog(v, NULL, NULL);
err_teardown_shutdown_varnishlog:
err_setup_start_varnishlog:
	return false;
}

static bool set_stream_buffering( FILE *stream, int mode, GError **err ) {
	if( setvbuf(stream, NULL, mode, 0) != 0 ) {
		g_set_error_errno(err);
		return false;
	}
	return true;
}

static gboolean set_buffer_mode( const gchar *option_name, const gchar *value, gpointer data, GError **err ) {
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

	char *qlfn = NULL;
	gint qlfd = -1;
	gboolean lowprio = false;

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
		{ "queue-length-file", 'q', 0, G_OPTION_ARG_FILENAME, &qlfn, "Write queue length as binary data to file", "file" },
		{ "low-priority", 'l', 0, G_OPTION_ARG_NONE, &lowprio, "Do not try to change to real-time priority", NULL },
		{ NULL, 0, 0, 0, NULL, NULL, NULL }
	};

	GOptionContext *option_context = g_option_context_new("- retrieve and queue varnish logs");
	g_option_context_add_main_entries(option_context, option_entries, NULL);

	if( !g_option_context_parse(option_context, &argc, &argv, &err) ) {
		crash = false;
		goto err_setup_option_error;
	}

	if( qlfn != NULL ) {
		qlfd = open(qlfn, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
		if( qlfd == -1 ) {
			g_set_error_errno(&err);
			goto err_setup_open_dev_zero;
		}

		if( truncate(qlfn, sizeof(gint)) == -1 ) {
			g_set_error_errno(&err);
			goto err_setup_truncate_qlfn;
		}

		int flags = fcntl(qlfd, F_GETFD);
		if( flags == -1 ) {
			g_set_error_errno(&err);
			goto err_setup_fcntl_qlfd_getfd;
		}

		if( fcntl(qlfd, F_SETFD, flags | FD_CLOEXEC) == -1 ) {
			g_set_error_errno(&err);
			goto err_setup_fcntl_qlfd_setfd;
		}
	}

	if( !reader_and_writer_main(qlfd, lowprio, &err) ) goto err_reader_and_writer_main;

	if( qlfn != NULL ) {
		if( close(qlfd) == -1 ) {
			g_set_error_errno(&err);
			goto err_teardown_close_qlfd;
		}

		if( unlink(qlfn) == -1 ) {
			g_set_error_errno(&err);
			goto err_teardown_unlink_qlfn;
		}

		g_free(qlfn);
	}

	g_option_context_free(option_context);

	return EXIT_SUCCESS;

err_reader_and_writer_main:
err_setup_fcntl_qlfd_setfd:
err_setup_fcntl_qlfd_getfd:
err_setup_truncate_qlfn:
	if( qlfn != NULL ) close(qlfd);
err_teardown_close_qlfd:
	if( qlfn != NULL ) unlink(qlfn);
err_teardown_unlink_qlfn:
err_setup_open_dev_zero:
	if( qlfn != NULL ) g_free(qlfn);
err_setup_option_error:
	g_option_context_free(option_context);

	if( crash ) {
		g_die(err);
	} else {
		fprintf(stderr, "%s\n", err->message);
		return EXIT_FAILURE;
	}
}
