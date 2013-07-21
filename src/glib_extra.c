#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

#include <glib.h>

#include "glib_extra.h"
#include "errors.h"

void g_set_error_errno( GError **err ) {
	int saved_errno = errno;
	g_set_error(
		err,
		ERRNO_QUARK,
		saved_errno,
		"%s",
		strerror(saved_errno)
	);
}

GString *g_string_wrap( gchar *bytes, gsize len, gsize allocated ) {
	// Here we manufacture a GString by creating one sized for 1 character
	// then freeing that 1 character and adding our data. We can't use
	// g_slice_new here because we don't know if g_string_free will use
	// g_slice_free or not. In fact manufacturing a GString like this is still
	// slightly risky as there may be undocumented fields in a GString. As of
	// glib 2.36.3 there are not however. The one other danger is if g_free ever
	// becomes incompatible with memory allocated using plain malloc, as that's
	// what g_string_free uses to free the character data, and malloc is what
	// getline uses to allocate the character data.
	GString *ret = g_string_sized_new(1);
	g_free(ret->str);
	ret->str = bytes;
	ret->len = len;
	ret->allocated_len = allocated;

	return ret;
}
