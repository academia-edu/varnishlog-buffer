#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>

#include <glib.h>

#include "common.h"
#include "die.h"

__attribute__((noreturn))
void _die_v( const char *fmt, va_list ap ) {
	fprintf(stderr, "%ju: ", (uintmax_t) getpid());
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
	g_assert(false); // Impossible
}

__attribute__((noreturn))
void _die( const char *fmt, ... ) {
	va_list ap;
	va_start(ap, fmt);
	_die_v(fmt, ap);
	g_assert(false); // Impossible
}

__attribute__((noreturn))
void die( const char *msg ) {
	_die("%s", msg);
}

__attribute__((noreturn))
void g_die( GError *err ) {
	if( err != NULL ) {
		die(err->message);
	} else {
		die("Unspecified error");
	}
}
