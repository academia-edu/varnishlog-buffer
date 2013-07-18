#include <errno.h>
#include <string.h>

#include <glib.h>

#include "glib_extra.h"
#include "errors.h"

void g_set_error_errno( GError **err ) {
	int saved_errno = errno;
	g_set_error(
		err,
		ACADEMIA_VARNISHLOG_ERRNO_QUARK,
		saved_errno,
		"%s",
		strerror(saved_errno)
	);
}
