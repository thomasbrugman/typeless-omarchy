#define _GNU_SOURCE
#include "json.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s;
    size_t n, i;
    int depth;
} jp;

static void ws(jp *p) {
    while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r'))
        p->i++;
}

static void jv_clear(jv *v);

static bool put_utf8(char **out, size_t *len, size_t *cap, unsigned cp) {
    char buf[4];
    size_t k;
    if (cp < 0x80) { buf[0] = (char)cp; k = 1; }
    else if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); k = 2; }
    else if (cp < 0x10000) { buf[0] = (char)(0xE0 | (cp >> 12)); buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[2] = (char)(0x80 | (cp & 0x3F)); k = 3; }
    else { buf[0] = (char)(0xF0 | (cp >> 18)); buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[3] = (char)(0x80 | (cp & 0x3F)); k = 4; }
    if (*len + k + 1 > *cap) {
        size_t nc = *cap ? *cap * 2 : 32;
        while (nc < *len + k + 1) nc *= 2;
        char *np = realloc(*out, nc);
        if (!np) return false;
        *out = np;
        *cap = nc;
    }
    memcpy(*out + *len, buf, k);
    *len += k;
    (*out)[*len] = 0;
    return true;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_str(jp *p, char **out) {
    if (p->i >= p->n || p->s[p->i] != '"') return false;
    p->i++;
    size_t len = 0, cap = 32;
    char *o = malloc(cap);
    if (!o) return false;
    o[0] = 0;
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i++];
        if (c == '"') {
            *out = o;
            return true;
        }
        if (c == '\\') {
            if (p->i >= p->n) break;
            char e = p->s[p->i++];
            unsigned cp;
            switch (e) {
            case 'n': cp = '\n'; break;
            case 't': cp = '\t'; break;
            case 'r': cp = '\r'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case 'u': {
                if (p->i + 4 > p->n) goto fail;
                cp = 0;
                for (int k = 0; k < 4; k++) {
                    int h = hexv(p->s[p->i + k]);
                    if (h < 0) goto fail;
                    cp = cp * 16 + (unsigned)h;
                }
                p->i += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p->i + 6 <= p->n && p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
                    unsigned lo = 0;
                    bool ok = true;
                    for (int k = 0; k < 4; k++) {
                        int h = hexv(p->s[p->i + 2 + k]);
                        if (h < 0) { ok = false; break; }
                        lo = lo * 16 + (unsigned)h;
                    }
                    if (ok && lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p->i += 6;
                    }
                }
                if (cp >= 0xD800 && cp < 0xE000) cp = 0xFFFD;
                break;
            }
            default: cp = (unsigned char)e; break;
            }
            if (!put_utf8(&o, &len, &cap, cp)) goto fail;
        } else {
            if (len + 2 > cap) {
                cap *= 2;
                char *np = realloc(o, cap);
                if (!np) goto fail;
                o = np;
            }
            o[len++] = (char)c;
            o[len] = 0;
        }
    }
fail:
    free(o);
    return false;
}

static bool parse_val(jp *p, jv *v);

static bool parse_container(jp *p, jv *v, bool obj) {
    if (++p->depth > 64) return false;
    p->i++; // [ or {
    v->t = obj ? JV_OBJ : JV_ARR;
    size_t cap = 0;
    ws(p);
    if (p->i < p->n && p->s[p->i] == (obj ? '}' : ']')) {
        p->i++;
        p->depth--;
        return true;
    }
    for (;;) {
        ws(p);
        char *key = NULL;
        if (obj) {
            if (!parse_str(p, &key)) return false;
            ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') { free(key); return false; }
            p->i++;
        }
        if (v->n == cap) {
            size_t nc = cap ? cap * 2 : 8;
            jv *ni = realloc(v->items, nc * sizeof(jv));
            if (!ni) { free(key); return false; }
            v->items = ni;
            if (obj) {
                char **nk = realloc(v->keys, nc * sizeof(char *));
                if (!nk) { free(key); return false; }
                v->keys = nk;
            }
            cap = nc;
        }
        memset(&v->items[v->n], 0, sizeof(jv));
        if (obj) v->keys[v->n] = key;
        v->n++;
        if (!parse_val(p, &v->items[v->n - 1])) return false;
        ws(p);
        if (p->i >= p->n) return false;
        char c = p->s[p->i++];
        if (c == ',') continue;
        if (c == (obj ? '}' : ']')) break;
        return false;
    }
    p->depth--;
    return true;
}

static bool parse_val(jp *p, jv *v) {
    ws(p);
    if (p->i >= p->n) return false;
    char c = p->s[p->i];
    if (c == '{') return parse_container(p, v, true);
    if (c == '[') return parse_container(p, v, false);
    if (c == '"') {
        v->t = JV_STR;
        return parse_str(p, &v->str);
    }
    if (p->n - p->i >= 4 && !strncmp(p->s + p->i, "true", 4)) {
        v->t = JV_BOOL; v->b = true; p->i += 4; return true;
    }
    if (p->n - p->i >= 5 && !strncmp(p->s + p->i, "false", 5)) {
        v->t = JV_BOOL; v->b = false; p->i += 5; return true;
    }
    if (p->n - p->i >= 4 && !strncmp(p->s + p->i, "null", 4)) {
        v->t = JV_NULL; p->i += 4; return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char buf[64];
        size_t k = 0;
        while (p->i < p->n && k < sizeof buf - 1 && strchr("+-0123456789.eE", p->s[p->i])) buf[k++] = p->s[p->i++];
        buf[k] = 0;
        char *end;
        v->t = JV_NUM;
        v->num = strtod(buf, &end);
        return end != buf;
    }
    return false;
}

static void jv_clear(jv *v) {
    if (!v) return;
    free(v->str);
    for (size_t i = 0; i < v->n; i++) {
        jv_clear(&v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
}

void jv_free(jv *v) {
    if (!v) return;
    jv_clear(v);
    free(v);
}

jv *jv_parse(const char *text, size_t len) {
    if (!text) return NULL;
    jp p = {text, len, 0, 0};
    jv *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    if (!parse_val(&p, v)) {
        jv_free(v);
        return NULL;
    }
    return v;
}

const jv *jv_get(const jv *obj, const char *key) {
    if (!obj || obj->t != JV_OBJ) return NULL;
    for (size_t i = 0; i < obj->n; i++)
        if (obj->keys[i] && !strcmp(obj->keys[i], key)) return &obj->items[i];
    return NULL;
}

const jv *jv_at(const jv *arr, size_t idx) {
    if (!arr || arr->t != JV_ARR || idx >= arr->n) return NULL;
    return &arr->items[idx];
}

const char *jv_str(const jv *v, const char *dflt) { return (v && v->t == JV_STR && v->str) ? v->str : dflt; }

double jv_num(const jv *v, double dflt) { return (v && v->t == JV_NUM) ? v->num : dflt; }

bool jv_bool(const jv *v, bool dflt) { return (v && v->t == JV_BOOL) ? v->b : dflt; }
