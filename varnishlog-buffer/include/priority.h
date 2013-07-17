#ifndef _PRIORITY_H_
#define _PRIORITY_H_

bool high_priority_process( int prio, GError **err );
bool high_priority_thread( int prio, GError **err );

bool set_thread_priority( pthread_t thread, int sched, int prio, GError **err );
bool swap_thread_priority( pthread_t thread, int sched, int prio, int *osched, int *oprio, GError **err );


#endif
