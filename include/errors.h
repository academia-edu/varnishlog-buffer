#ifndef _ERRORS_H_
#define _ERRORS_H_

typedef enum AcademiaVarnishLogError {
	ACADEMIA_VARNISHLOG_ERROR_EOF,
	ACADEMIA_VARNISHLOG_ERROR_UNSPEC
} AcademiaVarnishLogError;

#define ACADEMIA_VARNISHLOG_QUARK academia_varnishlog_quark()
#define ACADEMIA_VARNISHLOG_ERRNO_QUARK academia_varnishlog_errno_quark()

GQuark academia_varnishlog_quark();
GQuark academia_varnishlog_errno_quark();
bool write_gerror( GIOChannel *channel, GError *e, GError **err );
GError *read_gerror( GIOChannel *channel, GError **err );
void set_gerror_getline( FILE *, GError ** );

#endif
