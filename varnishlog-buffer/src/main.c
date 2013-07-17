#define _GNU_SOURCE

#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>

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

static volatile bool shutdown = false;

typedef struct SenderControl {
	GThread *thread;
	GSList *lines;
	GMutex lines_mutex;
	GCond lines_cond;
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

static void send_log_entry_to_rails( String *line ) {
	printf("%s\n", line->bytes);
	fflush(stdout);
}

static GError *rails_sender_main( SenderControl *control ) {
	pthread_t thread = pthread_self();
	GError *err = NULL;

	while( !control->shutdown ) {
		// Temporarily raise our priority for the duration of the critical section.
		int osched, oprio;
		if( !swap_thread_priority(thread, SCHED_FIFO, HIGH_THREAD_PRIORITY, &osched, &oprio, &err) )
			return err;

		g_mutex_lock(&control->lines_mutex);
		while( control->lines == NULL && !control->shutdown )
			g_cond_wait(&control->lines_cond, &control->lines_mutex);

		// Keep in mind lines may be NULL
		GSList *lines = control->lines;
		control->lines = NULL;
		g_mutex_unlock(&control->lines_mutex);

		if( !set_thread_priority(thread, osched, oprio, &err) )
			return err;

		lines = g_slist_reverse(lines);

		g_slist_foreach(lines, (GFunc) send_log_entry_to_rails, NULL);
		g_slist_free_full(lines, (GDestroyNotify) string_free);
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
	g_mutex_init(&sender_control.lines_mutex);
	g_cond_init(&sender_control.lines_cond);

	if( !high_priority_thread(HIGH_THREAD_PRIORITY, &err) ) goto out_high_priority_thread;

	while( !shutdown ) {
		char *line_bytes = NULL;
		size_t line_len;

restart_after_eintr:
		if( !read_varnishlog_entry(v, &line_bytes, &line_len, &err) ) {
			if( shutdown ) {
				g_clear_error(&err);
				break;
			} else if( err->domain == ACADEMIA_VARNISHLOG_ERRNO_QUARK && err->code == EINTR ) {
				// Retry if the syscall was interrupted.
				g_clear_error(&err);
				goto restart_after_eintr;
			}
			goto out_read_varnishlog_entry;
		}

		String *line = string_new_malloced_with_len(line_bytes, line_len);

		g_mutex_lock(&sender_control.lines_mutex);
		sender_control.lines = g_slist_prepend(sender_control.lines, line);
		g_cond_signal(&sender_control.lines_cond);
		g_mutex_unlock(&sender_control.lines_mutex);
	}

	sender_control.shutdown = true;

	// Wake up the other thread
	g_mutex_lock(&sender_control.lines_mutex);
	g_cond_broadcast(&sender_control.lines_cond);
	g_mutex_unlock(&sender_control.lines_mutex);

	err = g_thread_join(sender_control.thread);
	if( err != NULL ) goto out_g_thread_join;

	int stat;
	if( !shutdown_varnishlog(v, &stat, &err) ) goto out_shutdown_varnishlog;

	g_mutex_clear(&sender_control.lines_mutex);
	g_cond_clear(&sender_control.lines_cond);

	if( !WIFSIGNALED(stat) || WTERMSIG(stat) != SIGINT )
		return stat;

	return EXIT_SUCCESS;

out_shutdown_varnishlog:
out_g_thread_join:
out_read_varnishlog_entry:
out_high_priority_thread:
	sender_control.shutdown = true;

	g_mutex_lock(&sender_control.lines_mutex);
	if( sender_control.lines != NULL ) {
		g_slist_free_full(sender_control.lines, (GDestroyNotify) string_free);
		sender_control.lines = NULL;
	}
	g_cond_broadcast(&sender_control.lines_cond);
	g_mutex_unlock(&sender_control.lines_mutex);

	g_mutex_clear(&sender_control.lines_mutex);
	g_cond_clear(&sender_control.lines_cond);
out_register_signal_handlers:
	shutdown_varnishlog(v, NULL, NULL);
out_start_varnishlog:
	g_die(err);
}
