#ifndef _VARNISHLOG_H_
#define _VARNISHLOG_H_

typedef struct Varnishlog Varnishlog;

bool shutdown_varnishlog( Varnishlog *, int *stat, GError **err );
Varnishlog *start_varnishlog( GError **err );
bool read_varnishlog_entry( Varnishlog *, char **line, size_t *len, GError **err );

#endif
