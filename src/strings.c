#include <stdbool.h>
#include <stdlib.h>

#include <glib.h>

#include "strings.h"

String *string_new_malloced_with_len( char *bytes, size_t len ) {
	String *ret = g_slice_new(String);
	ret->should_free = true;
	ret->bytes = bytes;
	ret->len = len;
	return ret;
}

void string_free( String *st ) {
	if( st->should_free ) free(st->bytes);
	g_slice_free(String, st);
}
