/* clawc - json.h: minimal JSON DOM parser + writer. */
#ifndef CLAWC_JSON_H
#define CLAWC_JSON_H

#include "util.h"

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JVal JVal;
struct JVal {
    JType type;
    int boolean;
    double num;
    char *str;            /* J_STR */
    JVal **items;         /* J_ARR */
    int nitems, arrcap;
    char **keys;          /* J_OBJ */
    JVal **vals;
    int nkv, objcap;
};

JVal *json_parse(const char *s);          /* NULL on error */
void  json_free(JVal *v);

/* Object accessors (return NULL / dflt if missing or wrong type) */
JVal     *json_obj_get(const JVal *o, const char *key);
const char *json_get_str(const JVal *o, const char *key);
double    json_get_num(const JVal *o, const char *key, double dflt);

/* Writer helpers */
void json_quote(Buf *out, const char *s);   /* writes "escaped" including quotes */
void json_append_quoted(Buf *out, const char *key, const char *val);
void json_dump(const JVal *v, Buf *out);    /* serialize subtree */

#endif
