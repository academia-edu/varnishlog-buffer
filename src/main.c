#define _GNU_SOURCE

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

static volatile bool shutdown = false;

typedef struct SenderControl {
	GThread *thread;
	GSList *lines;
	volatile bool shutdown;
} SenderControl;

static void shutdown_sigaction() {
	shutdown = true;
}

static bool register_signal_handlers( GError **err ) {
	struct sigaction act;
	memset(&act, 0, sizeof(act));
	act.sa_handler = (void (*)( int )) shutdown_sigaction;

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

static void send_log_entry_to_rails( GString *line ) {
	printf("%s\n", line->str);
	fflush(stdout);
}

static void string_free( GString *str ) {
	g_string_free(str, true);
}

static GError *rails_sender_main( SenderControl *control ) {
	while( !control->shutdown ) {
		GSList *lines = (GSList *) g_atomic_pointer_and(&control->lines, 0);

		lines = g_slist_reverse(lines);

		g_slist_foreach(lines, (GFunc) send_log_entry_to_rails, NULL);
		g_slist_free_full(lines, (GDestroyNotify) string_free);

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
		.thread = g_thread_new("Rails Sender", (GThreadFunc) rails_sender_main, &sender_control),
		.lines = NULL,
		.shutdown = false
	};

	if( !high_priority_thread(HIGH_THREAD_PRIORITY, &err) ) goto out_high_priority_thread;

	while( !shutdown ) {
		GString *line = read_varnishlog_entry(v, &err);
		if( line == NULL ) {
			if( shutdown ) {
				g_clear_error(&err);
				break;
			} else if( err->domain == ACADEMIA_VARNISHLOG_ERRNO_QUARK && err->code == EINTR ) {
				// Retry if the syscall was interrupted.
				g_clear_error(&err);
				continue;
			}
			goto out_read_varnishlog_entry;
		}

		GSList *lines = (GSList *) g_atomic_pointer_and(&sender_control.lines, 0);
		lines = g_slist_prepend(lines, line);
		g_atomic_pointer_set(&sender_control.lines, lines);
	}

	sender_control.shutdown = true;

	err = g_thread_join(sender_control.thread);
	if( err != NULL ) goto out_g_thread_join;

	g_slist_foreach(sender_control.lines, (GFunc) send_log_entry_to_rails, NULL);
	g_slist_free_full(sender_control.lines, (GDestroyNotify) string_free);

	int stat;
	if( !shutdown_varnishlog(v, &stat, &err) ) goto out_shutdown_varnishlog;

	if( !WIFSIGNALED(stat) || WTERMSIG(stat) != SIGINT )
		return stat;

	return EXIT_SUCCESS;

out_shutdown_varnishlog:
out_g_thread_join:
out_read_varnishlog_entry:
out_high_priority_thread:
	sender_control.shutdown = true;

	g_slist_free_full(sender_control.lines, (GDestroyNotify) string_free);
out_register_signal_handlers:
	shutdown_varnishlog(v, NULL, NULL);
out_start_varnishlog:
	g_die(err);
}
