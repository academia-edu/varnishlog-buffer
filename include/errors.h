#ifndef _ERRORS_H_
#define _ERRORS_H_

typedef enum VarnishlogBufferError {
	VARNISHLOG_BUFFER_ERROR_EOF,
	VARNISHLOG_BUFFER_ERROR_QUEUE_SIZE,
	VARNISHLOG_BUFFER_ERROR_UNSPEC
} VarnishlogBufferError;

#define VARNISHLOG_BUFFER_QUARK varnishlog_buffer_quark()
#define ERRNO_QUARK errno_quark()

GQuark varnishlog_buffer_quark();
GQuark errno_quark();
bool write_gerror( GIOChannel *channel, GError *e, GError **err );
GError *read_gerror( GIOChannel *channel, GError **err );
void set_gerror_getline( FILE *, GError ** );

#endif
