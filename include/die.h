#ifndef _DIE_H_
#define _DIE_H_

__attribute__((noreturn))
void diefv( const char *fmt, va_list ap );

__attribute__((noreturn))
void dief( const char *fmt, ... );

__attribute__((noreturn))
void die( const char *msg );

__attribute__((noreturn))
void g_die( GError *err );

#endif
