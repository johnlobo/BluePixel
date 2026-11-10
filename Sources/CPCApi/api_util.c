#include "api_internal.h"
#define JSMN_STATIC
#include "jsmn.h"
#include "debug.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <errno.h>

int api_parse(Parser *p){
 jsmntok_t*raw=malloc(API_MAX_TOKENS*sizeof*raw);if(!raw)return 0;   /* heap, not stack: 190 KB would overflow a WebAssembly stack */
 jsmn_parser jp;jsmn_init(&jp);int n=jsmn_parse(&jp,p->s,strlen(p->s),raw,API_MAX_TOKENS);if(n<=0){free(raw);return 0;}p->n=n;
 for(int i=0;i<n;i++){int k=i+1;Tok*t=&p->t[k];t->type=raw[i].type==JSMN_OBJECT?'{':raw[i].type==JSMN_ARRAY?'[':raw[i].type==JSMN_STRING?'s':'v';t->start=raw[i].start-(t->type=='s');t->end=raw[i].end+(t->type=='s');int j=i+1;while(j<n&&raw[j].start<raw[i].end)j++;t->next=j+1;}
 free(raw);return 1;
}
int api_eq(Parser*p,int i,const char*s){return i>0&&p->t[i].type=='s'&&(int)strlen(s)==p->t[i].end-p->t[i].start-2&&!memcmp(p->s+p->t[i].start+1,s,strlen(s));}
int api_field(Parser*p,int obj,const char*key){if(!obj||p->t[obj].type!='{')return 0;for(int i=obj+1;i<p->t[obj].next;){int k=i,v=i+1;if(api_eq(p,k,key))return v;i=p->t[v].next;}return 0;}
/* Unescapes a JSON string (all escapes of RFC 8259, \uXXXX and surrogate pairs included) into malloc'd UTF-8.
 * A malformed escape (bad hex, lone surrogate) makes the whole string NULL, like a non-string. */
static int hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char ch = s[i];
        unsigned d = ch >= '0' && ch <= '9' ? (unsigned)(ch - '0') : ch >= 'a' && ch <= 'f' ? (unsigned)(ch - 'a' + 10) : ch >= 'A' && ch <= 'F' ? (unsigned)(ch - 'A' + 10) : 16u;
        if (d > 15) return 0;
        v = v << 4 | d;
    }
    *out = v;
    return 1;
}
char *api_str(Parser *p, int i) {
    if (!i || p->t[i].type != 's') return NULL;
    int first = p->t[i].start + 1, last = p->t[i].end - 1;            /* content between the quotes */
    char *r = malloc((size_t)(last - first) + 1);                      /* unescaping never grows the text */
    if (!r) return NULL;
    size_t j = 0;
    for (int x = first; x < last; x++) {
        char c = p->s[x];
        if (c != '\\') { r[j++] = c; continue; }
        if (++x >= last) { free(r); return NULL; }
        c = p->s[x];
        switch (c) {
            case 'n': r[j++] = '\n'; break;
            case 'r': r[j++] = '\r'; break;
            case 't': r[j++] = '\t'; break;
            case 'b': r[j++] = '\b'; break;
            case 'f': r[j++] = '\f'; break;
            case '"': case '\\': case '/': r[j++] = c; break;
            case 'u': {
                unsigned cp;
                if (x + 4 >= last) { free(r); return NULL; }
                if (!hex4(p->s + x + 1, &cp) || cp == 0) { free(r); return NULL; }   /* \u0000 would end the C string early */
                x += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {                     /* high surrogate: needs \uDC00..DFFF right after */
                    unsigned lo;
                    if (x + 6 >= last) { free(r); return NULL; }
                    if (p->s[x + 1] != '\\' || p->s[x + 2] != 'u' || !hex4(p->s + x + 3, &lo) || lo < 0xDC00 || lo > 0xDFFF) { free(r); return NULL; }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    x += 6;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) { free(r); return NULL; }
                if (cp < 0x80) r[j++] = (char)cp;
                else if (cp < 0x800) { r[j++] = (char)(0xC0 | cp >> 6); r[j++] = (char)(0x80 | (cp & 0x3F)); }
                else if (cp < 0x10000) { r[j++] = (char)(0xE0 | cp >> 12); r[j++] = (char)(0x80 | ((cp >> 6) & 0x3F)); r[j++] = (char)(0x80 | (cp & 0x3F)); }
                else { r[j++] = (char)(0xF0 | cp >> 18); r[j++] = (char)(0x80 | ((cp >> 12) & 0x3F)); r[j++] = (char)(0x80 | ((cp >> 6) & 0x3F)); r[j++] = (char)(0x80 | (cp & 0x3F)); }
                break;
            }
            default: free(r); return NULL;
        }
    }
    r[j] = 0;
    return r;
}
long api_num(Parser*p,int i,long d){if(!i)return d;int start=p->t[i].start+(p->t[i].type=='s'),end=p->t[i].end-(p->t[i].type=='s');int n=end-start;if(n<=0||n>=64)return d;char b[64];memcpy(b,p->s+start,(size_t)n);b[n]=0;char*e;errno=0;long v=strtol(b,&e,0);return *e||errno==ERANGE?d:v;}
bool api_boolean(Parser*p,int i,bool d){if(!i)return d;if(p->t[i].type=='v'){int n=p->t[i].end-p->t[i].start;if(n==4&&!memcmp(p->s+p->t[i].start,"true",4))return true;if(n==5&&!memcmp(p->s+p->t[i].start,"false",5))return false;}return d;}
long api_address_value(Parser*p,CPC*c,int i){if(!i)return-1;if(p->t[i].type!='s')return api_num(p,i,-1);char*expr=api_str(p,i);if(!expr)return-1;int32_t v=-1;char error[128];bool ok=dbg_eval(c,expr,&v,error,sizeof error);free(expr);return ok?v:-1;}
static const char B64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
unsigned char *api_unb64(const char*s,size_t*out){size_t n=strlen(s);if(n%4)return NULL;size_t pad=n&&s[n-1]=='='?1:0;if(n>1&&s[n-2]=='=')pad++;size_t cap=n/4*3-pad;unsigned char*b=malloc(cap?cap:1);if(!b)return NULL;size_t j=0;for(size_t i=0;i<n;i+=4){unsigned v=0;for(int k=0;k<4;k++){char ch=s[i+k];if(ch=='='){if(i+4!=n||k<2||(k==2&&s[i+3]!='=')){free(b);return NULL;}v<<=6;}else{const char*q=strchr(B64,ch);if(!q||i+k>=n-pad){free(b);return NULL;}v=(v<<6)|(unsigned)(q-B64);}}if(j<cap)b[j++]=(unsigned char)(v>>16);if(j<cap)b[j++]=(unsigned char)(v>>8);if(j<cap)b[j++]=(unsigned char)v;}*out=j;return b;}
char *api_b64(const unsigned char*b,size_t n){size_t cap=((n+2)/3)*4;char*r=malloc(cap+1);if(!r)return NULL;size_t j=0;for(size_t i=0;i<n;i+=3){unsigned v=(unsigned)b[i]<<16;if(i+1<n)v|=(unsigned)b[i+1]<<8;if(i+2<n)v|=b[i+2];r[j++]=B64[(v>>18)&63];r[j++]=B64[(v>>12)&63];r[j++]=i+1<n?B64[(v>>6)&63]:'=';r[j++]=i+2<n?B64[v&63]:'=';}r[j]=0;return r;}

void apibuf_printf(ApiBuf *b, const char *fmt, ...) {
    if (b->err) return;
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { b->err = true; va_end(ap2); return; }
    if (b->len + (size_t)n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + (size_t)n + 1) cap *= 2;
        char *nb = realloc(b->buf, cap);
        if (!nb) { b->err = true; va_end(ap2); return; }
        b->buf = nb;
        b->cap = cap;
    }
    vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
}

/* Length of the valid UTF-8 sequence at s (1..4), or 0 when the bytes are not well-formed (overlongs and surrogates included). */
static int utf8_len(const unsigned char *s) {
    if (s[0] < 0x80) return 1;
    if (s[0] >= 0xC2 && s[0] <= 0xDF) return (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (s[0] >= 0xE0 && s[0] <= 0xEF) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        unsigned cp = (unsigned)(s[0] & 0x0F) << 12 | (unsigned)(s[1] & 0x3F) << 6 | (s[2] & 0x3F);
        return cp >= 0x800 && (cp < 0xD800 || cp > 0xDFFF) ? 3 : 0;
    }
    if (s[0] >= 0xF0 && s[0] <= 0xF4) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        unsigned cp = (unsigned)(s[0] & 0x07) << 18 | (unsigned)(s[1] & 0x3F) << 12 | (unsigned)(s[2] & 0x3F) << 6 | (s[3] & 0x3F);
        return cp >= 0x10000 && cp <= 0x10FFFF ? 4 : 0;
    }
    return 0;
}

void apibuf_json_string(ApiBuf *b, const char *s) {
    apibuf_printf(b, "\"");
    const unsigned char *u = (const unsigned char *)(s ? s : "");
    while (*u) {
        unsigned char ch = *u;
        if (ch == '"' || ch == '\\') { apibuf_printf(b, "\\%c", ch); u++; }
        else if (ch < 0x20) { apibuf_printf(b, "\\u%04x", ch); u++; }
        else if (ch < 0x80) { apibuf_printf(b, "%c", ch); u++; }
        else {
            int n = utf8_len(u);
            if (!n) { apibuf_printf(b, "\\ufffd"); u++; }        /* not UTF-8 (a Latin-1 symbol map, say): a replacement character, not invalid JSON */
            else { for (int k = 0; k < n; k++) apibuf_printf(b, "%c", u[k]); u += n; }
        }
    }
    apibuf_printf(b, "\"");
}

char *apibuf_take(ApiBuf *b) {
    char *r = b->err ? NULL : b->buf;
    if (b->err) free(b->buf);
    if (!r && !b->err) r = strdup("");
    b->buf = NULL; b->len = b->cap = 0; b->err = false;
    return r;
}

/* Strict integer parameters. api_num falls back to a default on junk, which is right for optional knobs but wrong for
 * values that change state: there a missing, mistyped or fractional value must be an error, not a made-up number. */
int api_opt_int(Parser *p, int i, long dflt, long *out) {
    if (!i) { *out = dflt; return 1; }
    long lo = api_num(p, i, LONG_MIN), hi = api_num(p, i, LONG_MAX);
    if (lo != hi) return 0;                       /* the default came back, so the token is not an integer */
    *out = lo;
    return 1;
}
int api_req_int(Parser *p, int i, long *out) { return i && api_opt_int(p, i, 0, out); }
