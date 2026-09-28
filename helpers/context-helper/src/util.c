#define _GNU_SOURCE
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void sb_init(sb_t *b) { memset(b, 0, sizeof *b); }

void sb_free(sb_t *b) {
    free(b->p);
    memset(b, 0, sizeof *b);
}

static bool sb_reserve(sb_t *b, size_t extra) {
    if (b->oom) return false;
    size_t need = b->n + extra + 1;
    if (need <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) {
        b->oom = true;
        return false;
    }
    b->p = p;
    b->cap = cap;
    return true;
}

void sb_putn(sb_t *b, const char *s, size_t n) {
    if (!n || !sb_reserve(b, n)) return;
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void sb_puts(sb_t *b, const char *s) {
    if (s) sb_putn(b, s, strlen(s));
}

void sb_putc(sb_t *b, char c) { sb_putn(b, &c, 1); }

void sb_printf(sb_t *b, const char *fmt, ...) {
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) {
        sb_putn(b, tmp, (size_t)n);
        return;
    }
    if (!sb_reserve(b, (size_t)n)) return;
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)n;
}

// Returns length of a valid UTF-8 sequence starting at s (max n bytes), or 0.
static size_t utf8_valid_len(const unsigned char *s, size_t n) {
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    size_t len;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
    else return 0;
    if (len > n) return 0;
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return 0;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

void sb_json_escape_n(sb_t *b, const char *str, size_t n) {
    const unsigned char *s = (const unsigned char *)str;
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        if (c < 0x80) {
            switch (c) {
            case '"': sb_putn(b, "\\\"", 2); break;
            case '\\': sb_putn(b, "\\\\", 2); break;
            case '\b': sb_putn(b, "\\b", 2); break;
            case '\f': sb_putn(b, "\\f", 2); break;
            case '\n': sb_putn(b, "\\n", 2); break;
            case '\r': sb_putn(b, "\\r", 2); break;
            case '\t': sb_putn(b, "\\t", 2); break;
            default:
                if (c < 0x20 || c == 0x7f) sb_printf(b, "\\u%04x", c);
                else sb_putc(b, (char)c);
            }
            i++;
            continue;
        }
        size_t l = utf8_valid_len(s + i, n - i);
        if (!l) {
            sb_putn(b, "\xEF\xBF\xBD", 3);
            i++;
            continue;
        }
        // U+2028/U+2029 are valid JSON but escape them anyway for safety.
        if (l == 3 && s[i] == 0xE2 && s[i + 1] == 0x80 && (s[i + 2] == 0xA8 || s[i + 2] == 0xA9)) {
            sb_printf(b, "\\u%04x", s[i + 2] == 0xA8 ? 0x2028 : 0x2029);
        } else {
            sb_putn(b, (const char *)s + i, l);
        }
        i += l;
    }
}

void sb_json_escape(sb_t *b, const char *s) {
    if (s) sb_json_escape_n(b, s, strlen(s));
}

void sb_json_kv_str(sb_t *b, const char *key, const char *val) {
    sb_putc(b, '"');
    sb_puts(b, key);
    sb_puts(b, "\":\"");
    sb_json_escape(b, val ? val : "");
    sb_putc(b, '"');
}

char *sb_take(sb_t *b) {
    if (b->oom) {
        sb_free(b);
        return NULL;
    }
    if (!b->p) return strdup("");
    char *p = b->p;
    memset(b, 0, sizeof *b);
    return p;
}

int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

char *xstrdup(const char *s) { return strdup(s ? s : ""); }

char *xstrndup(const char *s, size_t n) { return strndup(s ? s : "", n); }

bool str_eq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}

bool str_has_prefix(const char *s, const char *pre) {
    return s && pre && strncmp(s, pre, strlen(pre)) == 0;
}

bool str_has_suffix(const char *s, const char *suf) {
    if (!s || !suf) return false;
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && memcmp(s + a - b, suf, b) == 0;
}

void str_lower_ascii(char *s) {
    for (; s && *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

size_t utf8_count(const char *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

size_t utf8_prefix_bytes(const char *s, size_t n, size_t max_cp) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (c == max_cp) return i;
            c++;
        }
    }
    return n;
}

void dbg(const char *fmt, ...) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("TYPELESS_CONTEXT_HELPER_DEBUG");
        enabled = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    if (!enabled) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[context-helper %lld] ", (long long)now_ms());
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
