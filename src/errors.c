#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#include <glib.h>

#include "common.h"
#include "errors.h"
#include "glib_extra.h"

GQuark varnishlog_buffer_quark() {
	return g_quark_from_static_string("varnishlog-buffer");
}

GQuark errno_quark() {
	return g_quark_from_static_string("errno");
}

static void set_error_eof( GError **err ) {
	g_set_error_literal(
		err,
		VARNISHLOG_BUFFER_QUARK,
		VARNISHLOG_BUFFER_ERROR_EOF,
		"Premature end of file"
	);
}

static void set_error_unspec( GError **err ) {
	g_set_error_literal(
		err,
		VARNISHLOG_BUFFER_QUARK,
		VARNISHLOG_BUFFER_ERROR_UNSPEC,
		"Unspecified error"
	);
}

bool write_gerror( GIOChannel *channel, GError *e, GError **err ) {
	GIOStatus status;
	gsize written;

	const gchar *domain = g_quark_to_string(e->domain);
	size_t domain_len = strlen(domain) + 1;

	status = g_io_channel_write_chars(channel, domain, domain_len, &written, err);
	g_assert(written == domain_len);
	g_assert(status != G_IO_STATUS_AGAIN);
	if( status != G_IO_STATUS_NORMAL ) goto out_error;

	status = g_io_channel_write_chars(channel, (gchar *) &e->code, sizeof(e->code), &written, err);
	g_assert(status != G_IO_STATUS_AGAIN);
	if( status != G_IO_STATUS_NORMAL ) goto out_error;

	size_t message_len = strlen(domain) + 1;
	status = g_io_channel_write_chars(channel, e->message, message_len, &written, err);
	g_assert(written == message_len);
	g_assert(status != G_IO_STATUS_AGAIN);
	if( status != G_IO_STATUS_NORMAL ) goto out_error;

	status = g_io_channel_flush(channel, err);
	g_assert(status != G_IO_STATUS_AGAIN);
	if( status != G_IO_STATUS_NORMAL ) goto out_error;

	return true;

out_error:
	if( status == G_IO_STATUS_EOF ) set_error_eof(err);
	return false;
}

GError *read_gerror( GIOChannel *channel, GError **err ) {
	GIOStatus status;
	g_io_channel_set_line_term(channel, "\0", 1);

	gchar *domain;
	status = g_io_channel_read_line(channel, &domain, NULL, NULL, err);
	g_assert(status != G_IO_STATUS_AGAIN);
	if( status != G_IO_STATUS_NORMAL ) goto out_read_domain;
	GQuark domain_quark = g_quark_from_string(domain);
	g_free(domain);

	gint code;
	gsize nread;
	status = g_io_channel_read_chars(channel, (gchar *) &code, sizeof(code), &nread, err);
	g_assert(status != G_IO_STATUS_AGAIN);
	if( status != G_IO_STATUS_NORMAL ) goto out_read_code;
	g_assert(nread == sizeof(code));

	gchar *message;
	status = g_io_channel_read_line(channel, &message, NULL, NULL, err);
	g_assert(status != G_IO_STATUS_AGAIN);
	if( status != G_IO_STATUS_NORMAL ) goto out_read_message;

	GError *ret = g_error_new_literal(domain_quark, code, "");
	g_free(ret->message);
	ret->message = message;

	return ret;

out_read_message:
out_read_code:
out_read_domain:
	if( status == G_IO_STATUS_EOF ) set_error_eof(err);
	return NULL;
}

void set_gerror_getline( FILE *strm, GError **err ) {
	if( errno != 0 ) {
		g_set_error_errno(err);
	} else if( feof(strm) ) {
		set_error_eof(err);
	} else {
		set_error_unspec(err);
	}
}
