#include <stdbool.h>
#include <pthread.h>
#include <sched.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include "common.h"
#include "glib_extra.h"
#include "priority.h"

bool high_priority_process( int prio, GError **err ) {
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

bool set_thread_priority( pthread_t thread, int sched, int prio, GError **err ) {
	struct sched_param param;
	memset(&param, 0, sizeof(param));
	param.sched_priority = prio;
	if( (errno = pthread_setschedparam(thread, sched, &param)) != 0 ) {
		g_set_error_errno(err);
		return false;
	}
	return true;
}

bool high_priority_thread( int prio, GError **err ) {
	return set_thread_priority(pthread_self(), SCHED_FIFO, prio, err);
}
