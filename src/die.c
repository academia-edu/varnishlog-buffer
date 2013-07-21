#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdbool.h>

#include <glib.h>

#include "common.h"
#include "die.h"

__attribute__((noreturn))
void diefv( const char *fmt, va_list ap ) {
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
	g_assert(false); // Impossible
}

__attribute__((noreturn))
void dief( const char *fmt, ... ) {
	va_list ap;
	va_start(ap, fmt);
	diefv(fmt, ap);
	g_assert(false); // Impossible
}

__attribute__((noreturn))
void die( const char *msg ) {
	dief("%s", msg);
}

__attribute__((noreturn))
void g_die( GError *err ) {
	if( err != NULL ) {
		dief("%s (%d): %s", g_quark_to_string(err->domain), err->code, err->message);
	} else {
		die("Unspecified error");
	}
}
