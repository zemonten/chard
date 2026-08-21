#include "../../chard.h"

int in_string_at(const char *line_start, const char *p) {
    int in_str = 0;
    for (const char *s = line_start; s < p; s++) {
        if (*s == '\\' && in_str && s + 1 < p) { s++; continue; }
        if (*s == '"') in_str = !in_str;
    }
    return in_str;
}

const char *find_string_close_quote(const char *q1) {
    const char *p = q1 + 1;
    while (*p) {
        if (*p == '\\' && p[1] != '\0') { p += 2; continue; }
        if (*p == '"') return p;
        p++;
    }
    return NULL;
}

char *strip__comment(char *line) {
    char *c = line;
    while ((c = strstr(c, "//")) != NULL) {
        if (!in_string_at(line, c)) { *c = '\0'; break; }
        c += 2;
    }
    return line;
}

char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

char *str_dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) { perror("malloc"); exit(1); }
    memcpy(p, s, n);
    return p;
}

int is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

size_t buf__append(char *dst, size_t cap, size_t len, const char *src, size_t srclen) {
    if (len >= cap) return len;
    size_t room = cap - len - 1;
    if (srclen > room) srclen = room;
    memcpy(dst + len, src, srclen);
    return len + srclen;
}

int is__decimal_number(const char *tok) {
    if (!*tok) return 0;
    const char *p = tok;
    if (*p == '-') p++;
    if (!*p) return 0;
    while (*p) { if (!isdigit((unsigned char)*p)) return 0; p++; }
    return 1;
}

int is__hex_number(const char *tok) {
    const char *p = tok;
    if (*p == '-') p++;
    if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) return 0;
    p += 2;
    if (!*p) return 0;
    while (*p) { if (!isxdigit((unsigned char)*p)) return 0; p++; }
    return 1;
}

int is__binary_number(const char *tok) {
    const char *p = tok;
    if (*p == '-') p++;
    if (p[0] != '0' || (p[1] != 'b' && p[1] != 'B')) return 0;
    p += 2;
    if (!*p) return 0;
    while (*p) { if (*p != '0' && *p != '1') return 0; p++; }
    return 1;
}

int is__number(const char *tok) {
    return is__decimal_number(tok) || is__hex_number(tok) || is__binary_number(tok);
}

int is_float_literal(const char *tok) {
    const char *p = tok;
    if (*p == '-') p++;
    int int_digits = 0, frac_digits = 0;
    while (isdigit((unsigned char)*p)) { p++; int_digits++; }
    if (*p != '.') return 0;
    p++;
    while (isdigit((unsigned char)*p)) { p++; frac_digits++; }
    return *p == '\0' && int_digits > 0 && frac_digits > 0;
}

void strip__semicolon(char *tok) {
    size_t len = strlen(tok);
    if (len > 0 && tok[len - 1] == ';') tok[len - 1] = '\0';
}

void strip__trailing_comma(char *tok) {
    size_t len = strlen(tok);
    if (len > 0 && tok[len - 1] == ',') tok[len - 1] = '\0';
}

mem_order_t parse_mem_order_suffix(const char *raw_trimmed, const char *opname) {
    const char *pct = strchr(raw_trimmed, '%');
    if (!pct) return MEM_ORDER_SEQ_CST;
    const char *p = pct + 1;
    static const struct { const char *name; mem_order_t order; } orders[] = {
        {"seq_cst", MEM_ORDER_SEQ_CST},
        {"relaxed", MEM_ORDER_RELAXED},
        {"acquire", MEM_ORDER_ACQUIRE},
        {"release", MEM_ORDER_RELEASE},
        {"acq_rel", MEM_ORDER_ACQ_REL},
    };
    for (size_t i = 0; i < sizeof(orders)/sizeof(orders[0]); i++) {
        size_t nlen = strlen(orders[i].name);
        if (strncmp(p, orders[i].name, nlen) == 0 && !is_ident_char(p[nlen]))
            return orders[i].order;
    }
    failf("'%s': unrecognized memory ordering after '%%' -- expected one of %%relaxed, %%acquire, %%release, %%acq_rel, %%seq_cst", opname);
    return MEM_ORDER_SEQ_CST; /* unreachable, failf() never returns -- silences -Wreturn-type */
}

int has_extension(const char *path, const char *ext) {
    size_t plen = strlen(path), elen = strlen(ext);
    if (elen > plen) return 0;
    return strcmp(path + (plen - elen), ext) == 0;
}

int stack_slot_size(target_t t) {
    return target_defs[t].stack_slot_bytes;
}

