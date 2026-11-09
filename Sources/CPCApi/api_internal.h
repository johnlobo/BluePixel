#ifndef BLUEPIXEL_API_INTERNAL_H
#define BLUEPIXEL_API_INTERNAL_H
/* Internal to CPCApi: request parsing helpers and the session shared by api.c and the api_*.c method modules. */
#include "cpc_api.h"
#include "cpc.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Token slots are 1-based (slot 0 means "absent"), so n tokens need n + 1 slots. */
#define API_MAX_TOKENS 8192
typedef struct { int start, end, next; char type; } Tok;
typedef struct { const char *s; Tok t[API_MAX_TOKENS + 1]; int n; } Parser;

int api_parse(Parser *p);
int api_eq(Parser *p, int i, const char *s);
int api_field(Parser *p, int obj, const char *key);
char *api_str(Parser *p, int i);                        /* malloc'd, unescaped; NULL when not a string */
long api_num(Parser *p, int i, long dflt);
int api_opt_int(Parser *p, int i, long dflt, long *out);   /* 1 when absent (dflt) or a valid integer; 0 when present but not one */
int api_req_int(Parser *p, int i, long *out);              /* 1 only for a present, valid integer */
bool api_boolean(Parser *p, int i, bool dflt);
long api_address_value(Parser *p, CPC *c, int i);        /* number or debugger expression; -1 when invalid */
unsigned char *api_unb64(const char *s, size_t *out);    /* NULL when malformed */
char *api_b64(const unsigned char *b, size_t n);

struct CPCApiSession { CPC *c; int boot_target; int boot_configured; bool rom_os, rom_basic, rom_amsdos; };

/* Growable string for building JSON results. After an allocation failure every call is a no-op and sb.err is set. */
typedef struct { char *buf; size_t len, cap; bool err; } ApiBuf;
void apibuf_printf(ApiBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void apibuf_json_string(ApiBuf *b, const char *s);       /* quoted, JSON-escaped */
char *apibuf_take(ApiBuf *b);                            /* hands over the buffer (NULL after an error); resets b */

/* A method module returns 1 when it owns `method`. On success *result is a malloc'd JSON value and the return is 1 with
 * *error == NULL; on failure *error is a static message. */
int api_media_dispatch(CPCApiSession *s, Parser *p, int params, const char *method, char **result, const char **error);
int api_debug_dispatch(CPCApiSession *s, Parser *p, int params, const char *method, char **result, const char **error);
#endif
