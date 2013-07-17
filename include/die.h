#ifndef _DIE_H_
#define _DIE_H_

__attribute__((noreturn))
void _die_v( const char *fmt, va_list ap );

__attribute__((noreturn))
void _die( const char *fmt, ... );

__attribute__((noreturn))
void die( const char *msg );

__attribute__((noreturn))
void g_die( GError *err );

#define dief(fmt, ...) (_die((fmt), ## __VA_ARGS__))

#endif
