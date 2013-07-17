#ifndef _ERRORS_H_
#define _ERRORS_H_

#include "common.h"

#include <glib.h>

typedef enum AcademiaVarnishLogError {
	ACADEMIA_VARNISHLOG_ERROR_EOF,
	ACADEMIA_VARNISHLOG_ERROR_UNSPEC
} AcademiaVarnishLogError;

#define ACADEMIA_VARNISHLOG_QUARK academia_varnishlog_quark()
#define ACADEMIA_VARNISHLOG_ERRNO_QUARK academia_varnishlog_errno_quark()

GQuark academia_varnishlog_quark();
GQuark academia_varnishlog_errno_quark();

#endif
