#include "../../../chard.h"

long parse__number(const char *tok) {
    int neg = 0;
    const char *p = tok;
    if (*p == '-') { neg = 1; p++; }
    long v;
    if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        v = strtol(p + 2, NULL, 2);
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        v = strtol(p, NULL, 16);
    } else {
        v = strtol(p, NULL, 10);
    }
    return neg ? -v : v;
}

const char *resolve_size_alias(const char *tok) {
    if (strcmp(tok, "char") == 0) return "i8";
    if (strcmp(tok, "short") == 0) return "i16";
    if (strcmp(tok, "int") == 0) return "i32";
    if (strcmp(tok, "long") == 0) return "i64";
    return tok;
}

uint64_t double__bits(double d) {
    union { double d; uint64_t u; } v;
    v.d = d;
    return v.u;
}

uint32_t float__bits(double d) {
    union { float f; uint32_t u; } v;
    v.f = (float)d;
    return v.u;
}

