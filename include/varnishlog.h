#ifndef _VARNISHLOG_H_
#define _VARNISHLOG_H_

typedef struct Varnishlog Varnishlog;

bool shutdown_varnishlog( Varnishlog *, int *stat, GError **err );
Varnishlog *start_varnishlog( gboolean, GError **err );
GString *read_varnishlog_entry( Varnishlog *v, GError **err );

#endif
