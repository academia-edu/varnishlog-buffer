#ifndef _STRING_H_
#define _STRING_H_

typedef struct String {
	char *bytes;
	size_t len;
	bool should_free;
} String;

String *string_new_malloced_with_len( char *, size_t );
void string_free( String * );

#endif
