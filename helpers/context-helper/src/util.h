// Small shared utilities for the clean-room libContextHelper (Linux arm64).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// Growable string buffer
// ---------------------------------------------------------------------------
typedef struct {
    char *p;
    size_t n;
    size_t cap;
    bool oom;
} sb_t;

void sb_init(sb_t *b);
void sb_free(sb_t *b);
void sb_putn(sb_t *b, const char *s, size_t n);
void sb_puts(sb_t *b, const char *s);
void sb_putc(sb_t *b, char c);
void sb_printf(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// Append `s` escaped for use inside a JSON string literal (no surrounding
// quotes). Invalid UTF-8 is replaced by U+FFFD so JSON.parse never chokes.
void sb_json_escape(sb_t *b, const char *s);
void sb_json_escape_n(sb_t *b, const char *s, size_t n);
// "key":"escaped value"
void sb_json_kv_str(sb_t *b, const char *key, const char *val);
// Detach the buffer as a malloc()ed NUL-terminated string (never NULL unless OOM).
char *sb_take(sb_t *b);

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------
int64_t now_ms(void);
char *xstrdup(const char *s);              // strdup that tolerates NULL (returns "")
char *xstrndup(const char *s, size_t n);
bool str_eq(const char *a, const char *b); // NULL-safe
bool str_has_prefix(const char *s, const char *pre);
bool str_has_suffix(const char *s, const char *suf);
void str_lower_ascii(char *s);

// UTF-8 helpers (tolerant of malformed input)
size_t utf8_count(const char *s, size_t n);          // code points in s[0..n)
size_t utf8_prefix_bytes(const char *s, size_t n, size_t max_cp); // bytes of first max_cp code points

// Debug logging, enabled with TYPELESS_CONTEXT_HELPER_DEBUG=1 (stderr).
void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
