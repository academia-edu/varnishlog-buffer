#ifndef _GLIB_EXTRA_H_
#define _GLIB_EXTRA_H_

void g_set_error_errno( GError **err );
GString *g_string_wrap( gchar *, gsize, gsize );

#endif
