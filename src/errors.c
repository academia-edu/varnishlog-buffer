#include "errors.h"

#include <glib.h>

GQuark academia_varnishlog_quark() {
	return g_quark_from_static_string("academia-varnishlog-quark");
}

GQuark academia_varnishlog_errno_quark() {
	return g_quark_from_static_string("academia-varnishlog-errno-quark");
}
