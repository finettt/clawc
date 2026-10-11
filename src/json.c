/* clawc - json.c: recursive-descent JSON parser + serializer. */
#include "json.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    int depth;
} Parser;

static JVal *parse_value(Parser *ps);

static JVal *jnew(JType t) {
    JVal *v = xmalloc(sizeof *v);
    memset(v, 0, sizeof *v);
    v->type = t;
    return v;
}

void json_free(JVal *v) {
    if (!v) return;
    int i;
    if (v->type == J_STR) free(v->str);
    if (v->type == J_ARR) {
        for (i = 0; i < v->nitems; i++) json_free(v->items[i]);
        free(v->items);
    }
    if (v->type == J_OBJ) {
        for (i = 0; i < v->nkv; i++) { free(v->keys[i]); json_free(v->vals[i]); }
        free(v->keys);
        free(v->vals);
    }
    free(v);
}

static void skip_ws(Parser *ps) {
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') ps->p++;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void utf8_encode(Buf *b, unsigned cp) {
    if (cp < 0x80) {
        char c = (char)cp;
        buf_add(b, &c, 1);
    } else if (cp < 0x800) {
        char c[2] = {(char)(0xC0 | (cp >> 6)), (char)(0x80 | (cp & 0x3F))};
        buf_add(b, c, 2);
    } else if (cp < 0x10000) {
        char c[3] = {(char)(0xE0 | (cp >> 12)), (char)(0x80 | ((cp >> 6) & 0x3F)),
                     (char)(0x80 | (cp & 0x3F))};
        buf_add(b, c, 3);
    } else {
        char c[4] = {(char)(0xF0 | (cp >> 18)), (char)(0x80 | ((cp >> 12) & 0x3F)),
                     (char)(0x80 | ((cp >> 6) & 0x3F)), (char)(0x80 | (cp & 0x3F))};
        buf_add(b, c, 4);
    }
}

static char *parse_string_raw(Parser *ps) { /* ps->p at opening quote */
    ps->p++;
    Buf b;
    buf_init(&b);
    while (*ps->p && *ps->p != '"') {
        char c = *ps->p;
        if (c == '\\') {
            ps->p++;
            char e = *ps->p;
            switch (e) {
            case 'n': buf_add(&b, "\n", 1); break;
            case 't': buf_add(&b, "\t", 1); break;
            case 'r': buf_add(&b, "\r", 1); break;
            case 'b': buf_add(&b, "\b", 1); break;
            case 'f': buf_add(&b, "\f", 1); break;
            case '"': buf_add(&b, "\"", 1); break;
            case '\\': buf_add(&b, "\\", 1); break;
            case '/': buf_add(&b, "/", 1); break;
            case 'u': {
                unsigned cp = 0;
                int i, ok = 1;
                for (i = 0; i < 4; i++) {
                    int h = hexval(ps->p[1 + i]);
                    if (h < 0) { ok = 0; break; }
                    cp = (cp << 4) | (unsigned)h;
                }
                if (!ok) { buf_free(&b); return NULL; }
                ps->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && ps->p[1] == '\\' && ps->p[2] == 'u') {
                    unsigned lo = 0;
                    int i2, ok2 = 1;
                    for (i2 = 0; i2 < 4; i2++) {
                        int h = hexval(ps->p[3 + i2]);
                        if (h < 0) { ok2 = 0; break; }
                        lo = (lo << 4) | (unsigned)h;
                    }
                    if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        ps->p += 6;
                    }
                }
                utf8_encode(&b, cp);
                break;
            }
            default: buf_free(&b); return NULL;
            }
            ps->p++;
        } else {
            buf_add(&b, &c, 1);
            ps->p++;
        }
    }
    if (*ps->p != '"') { buf_free(&b); return NULL; }
    ps->p++;
    return b.s;
}

static JVal *parse_value(Parser *ps) {
    if (++ps->depth > 64) return NULL;
    skip_ws(ps);
    JVal *v = NULL;
    char c = *ps->p;
    if (c == '{') {
        v = jnew(J_OBJ);
        ps->p++;
        skip_ws(ps);
        if (*ps->p == '}') {
            ps->p++; /* empty object {} */
        } else {
            for (;;) {
                skip_ws(ps);
                if (*ps->p != '"') { json_free(v); ps->depth--; return NULL; }
                char *key = parse_string_raw(ps);
                if (!key) { json_free(v); ps->depth--; return NULL; }
                skip_ws(ps);
                if (*ps->p != ':') { free(key); json_free(v); ps->depth--; return NULL; }
                ps->p++;
                JVal *val = parse_value(ps);
                if (!val) { free(key); json_free(v); ps->depth--; return NULL; }
                if (v->nkv + 1 > v->objcap) {
                    v->objcap = v->objcap ? v->objcap * 2 : 8;
                    v->keys = xrealloc(v->keys, (size_t)v->objcap * sizeof *v->keys);
                    v->vals = xrealloc(v->vals, (size_t)v->objcap * sizeof *v->vals);
                }
                v->keys[v->nkv] = key;
                v->vals[v->nkv] = val;
                v->nkv++;
                skip_ws(ps);
                if (*ps->p == ',') { ps->p++; continue; }
                break;
            }
            skip_ws(ps);
            if (*ps->p != '}') { json_free(v); ps->depth--; return NULL; }
            ps->p++;
        }
    } else if (c == '[') {
        v = jnew(J_ARR);
        ps->p++;
        skip_ws(ps);
        if (*ps->p == ']') { ps->p++; }
        else {
            for (;;) {
                JVal *item = parse_value(ps);
                if (!item) { json_free(v); ps->depth--; return NULL; }
                if (v->nitems + 1 > v->arrcap) {
                    v->arrcap = v->arrcap ? v->arrcap * 2 : 8;
                    v->items = xrealloc(v->items, (size_t)v->arrcap * sizeof *v->items);
                }
                v->items[v->nitems++] = item;
                skip_ws(ps);
                if (*ps->p == ',') { ps->p++; continue; }
                break;
            }
            if (*ps->p != ']') { json_free(v); ps->depth--; return NULL; }
            ps->p++;
        }
    } else if (c == '"') {
        char *s = parse_string_raw(ps);
        if (!s) { ps->depth--; return NULL; }
        v = jnew(J_STR);
        v->str = s;
    } else if (c == 't' && strncmp(ps->p, "true", 4) == 0) {
        v = jnew(J_BOOL);
        v->boolean = 1;
        ps->p += 4;
    } else if (c == 'f' && strncmp(ps->p, "false", 5) == 0) {
        v = jnew(J_BOOL);
        ps->p += 5;
    } else if (c == 'n' && strncmp(ps->p, "null", 4) == 0) {
        v = jnew(J_NULL);
        ps->p += 4;
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        const char *sp = ps->p;
        v = jnew(J_NUM);
        v->num = scan_num(&ps->p);
        if (ps->p == sp) { json_free(v); ps->depth--; return NULL; }
    } else {
        ps->depth--;
        return NULL;
    }
    ps->depth--;
    return v;
}

JVal *json_parse(const char *s) {
    Parser ps = {s, 0};
    JVal *v = parse_value(&ps);
    return v;
}

JVal *json_obj_get(const JVal *o, const char *key) {
    if (!o || o->type != J_OBJ) return NULL;
    int i;
    for (i = 0; i < o->nkv; i++)
        if (strcmp(o->keys[i], key) == 0) return o->vals[i];
    return NULL;
}

const char *json_get_str(const JVal *o, const char *key) {
    JVal *v = json_obj_get(o, key);
    return (v && v->type == J_STR) ? v->str : NULL;
}

double json_get_num(const JVal *o, const char *key, double dflt) {
    JVal *v = json_obj_get(o, key);
    return (v && v->type == J_NUM) ? v->num : dflt;
}

void json_quote(Buf *out, const char *s) {
    buf_puts(out, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"': buf_puts(out, "\\\""); break;
        case '\\': buf_puts(out, "\\\\"); break;
        case '\n': buf_puts(out, "\\n"); break;
        case '\r': buf_puts(out, "\\r"); break;
        case '\t': buf_puts(out, "\\t"); break;
        case '\b': buf_puts(out, "\\b"); break;
        case '\f': buf_puts(out, "\\f"); break;
        default:
            if (c < 0x20) buf_printf(out, "\\u%04x", c);
            else buf_add(out, &c, 1);
        }
    }
    buf_puts(out, "\"");
}

void json_dump(const JVal *v, Buf *out) {
    int i;
    if (!v) { buf_puts(out, "null"); return; }
    switch (v->type) {
    case J_NULL: buf_puts(out, "null"); break;
    case J_BOOL: buf_puts(out, v->boolean ? "true" : "false"); break;
    case J_NUM:
        if (v->num == (double)(long long)v->num && v->num > -1e15 && v->num < 1e15)
            buf_printf(out, "%lld", (long long)v->num);
        else
            buf_printf(out, "%.17g", v->num);
        break;
    case J_STR: json_quote(out, v->str ? v->str : ""); break;
    case J_ARR:
        buf_puts(out, "[");
        for (i = 0; i < v->nitems; i++) {
            if (i) buf_puts(out, ",");
            json_dump(v->items[i], out);
        }
        buf_puts(out, "]");
        break;
    case J_OBJ:
        buf_puts(out, "{");
        for (i = 0; i < v->nkv; i++) {
            if (i) buf_puts(out, ",");
            json_quote(out, v->keys[i]);
            buf_puts(out, ":");
            json_dump(v->vals[i], out);
        }
        buf_puts(out, "}");
        break;
    }
}
