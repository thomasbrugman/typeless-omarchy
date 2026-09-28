// Minimal JSON reader (enough for Hyprland IPC replies).
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ } jv_type;

typedef struct jv {
    jv_type t;
    bool b;
    double num;
    char *str;           // JV_STR (decoded, NUL-terminated)
    struct jv *items;    // JV_ARR / JV_OBJ values
    char **keys;         // JV_OBJ keys
    size_t n;
} jv;

// Parses `text`; returns NULL on syntax error. Free with jv_free().
jv *jv_parse(const char *text, size_t len);
void jv_free(jv *v);

const jv *jv_get(const jv *obj, const char *key);    // NULL if missing / not an object
const jv *jv_at(const jv *arr, size_t idx);          // NULL if missing / not an array
const char *jv_str(const jv *v, const char *dflt);
double jv_num(const jv *v, double dflt);
bool jv_bool(const jv *v, bool dflt);
