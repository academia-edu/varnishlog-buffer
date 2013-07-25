#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>

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

static GError *sender_main( SenderControl *control ) {
	while( true ) {
		GSList *lines = (GSList *) g_atomic_pointer_and(&control->lines, 0);

		lines = g_slist_reverse(lines);

		GError *err = NULL;
		g_slist_foreach(lines, (GFunc) print_log_entry, &err);
		g_slist_free_full(lines, (GDestroyNotify) string_free);

		if( err != NULL ) return err;

		if( g_atomic_int_get(&control->shutdown) && g_atomic_pointer_get(&control->lines) == NULL )
			break;

		usleep(SENDER_SLEEP_NS);
	}

	return NULL;
}

int main() {
	GError *err = NULL;

	Varnishlog *v = start_varnishlog(&err);
	if( v == NULL ) goto out_start_varnishlog;
	if( !register_signal_handlers(&err) ) goto out_register_signal_handlers;

	SenderControl sender_control = {
		.lines = NULL,
		.shutdown = false
	};
	// Note that sender_control.thread might not be initialized when
	// the thread starts.
	sender_control.thread = g_thread_new("Rails Sender", (GThreadFunc) sender_main, &sender_control);

	if( !high_priority_thread(HIGH_THREAD_PRIORITY, &err) ) goto out_high_priority_thread;

	while( !g_atomic_int_get(&shutdown) ) {
		GString *line = read_varnishlog_entry(v, &err);

		if( line != NULL ) {
			GSList *lines = (GSList *) g_atomic_pointer_and(&sender_control.lines, 0);
			lines = g_slist_prepend(lines, line);
			g_atomic_pointer_set(&sender_control.lines, lines);
		}

		if( err != NULL ) {
			if( g_atomic_int_get(&shutdown) ) {
				g_clear_error(&err);
				break;
			} else if( err->domain == ERRNO_QUARK && err->code == EINTR ) {
				// Retry if the syscall was interrupted.
				g_clear_error(&err);
				continue;
			}
			goto out_read_varnishlog_entry;
		}
	}

	// This is just in case the call to ignore SIGPIPE in the signal handler
	// failed as that call isn't checked.
	if( signal(SIGPIPE, SIG_IGN) == SIG_ERR ) {
		g_set_error_errno(&err);
		goto out_signal_sigpipe;
	}

	g_atomic_int_set(&sender_control.shutdown, true);

	err = g_thread_join(sender_control.thread);
	if( err != NULL ) goto out_g_thread_join;

	g_assert_cmpuint(g_slist_length(sender_control.lines), ==, 0);

	int stat;
	if( !shutdown_varnishlog(v, &stat, &err) ) goto out_shutdown_varnishlog;

	if( !WIFSIGNALED(stat) || WTERMSIG(stat) != SIGINT )
		return stat;

	return EXIT_SUCCESS;

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
	g_die(err);
}
