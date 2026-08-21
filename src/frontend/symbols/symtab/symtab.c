#include "../../../chard.h"

equ_def_t *find__equ(const char *name) {
    for (int i = 0; i < nequs; i++)
        if (strcmp(equs[i].name, name) == 0) return &equs[i];
    return NULL;
}

alias_def_t *find__alias(const char *name) {
    for (int i = 0; i < naliases; i++)
        if (strcmp(aliases[i].name, name) == 0) return &aliases[i];
    return NULL;
}

enum_member_t *find_enum_member(const char *qualified_name) {
    for (int i = 0; i < nenum_members; i++)
        if (strcmp(enum_members[i].qualified_name, qualified_name) == 0) return &enum_members[i];
    return NULL;
}

int enum_name_already_declared(const char *enum_name) {
    size_t nlen = strlen(enum_name);
    for (int i = 0; i < nenum_members; i++) {
        const char *qn = enum_members[i].qualified_name;
        if (strncmp(qn, enum_name, nlen) == 0 && qn[nlen] == '.') return 1;
    }
    return 0;
}

struct_def_t *find_struct_def(const char *name) {
    for (int i = 0; i < nstruct_defs; i++)
        if (strcmp(struct_defs[i].name, name) == 0) return &struct_defs[i];
    return NULL;
}

struct_field_t *find_struct_field(struct_def_t *sd, const char *field_name) {
    for (int i = 0; i < sd->nfields; i++)
        if (strcmp(sd->fields[i].name, field_name) == 0) return &sd->fields[i];
    return NULL;
}

void enum_finish_member(enum_collect_t *ec, const char *member_text) {
    /* member_text is one comma-separated piece, already trimmed, of the
       form 'NAME' or 'NAME = VALUE', possibly with a trailing '}'/';'
       already stripped by the caller. */
    char name[MAX_SYMLEN];
    long value;
    const char *eq = strchr(member_text, '=');
    if (eq) {
        size_t namelen = (size_t)(eq - member_text);
        while (namelen > 0 && isspace((unsigned char)member_text[namelen - 1])) namelen--;
        if (namelen == 0 || namelen >= MAX_SYMLEN)
            failf("enum '%s': malformed member (missing name before '=')", ec->name);
        memcpy(name, member_text, namelen);
        name[namelen] = '\0';
        char valbuf[64];
        const char *vp = eq + 1;
        while (isspace((unsigned char)*vp)) vp++;
        const char *vend = vp;
        while (*vend && !isspace((unsigned char)*vend)) vend++;
        size_t vlen = (size_t)(vend - vp);
        if (vlen == 0 || vlen >= sizeof(valbuf)) {
            char msg[192];
            snprintf(msg, sizeof(msg), "enum '%s.%s': expected an integer value after '='", ec->name, name);
            fail(msg);
        }
        memcpy(valbuf, vp, vlen);
        valbuf[vlen] = '\0';
        if (!is__number(valbuf)) {
            char msg[192];
            snprintf(msg, sizeof(msg), "enum '%s.%s': explicit value must be a plain integer literal", ec->name, name);
            fail(msg);
        }
        value = parse__number(valbuf);
    } else {
        strncpy(name, member_text, MAX_SYMLEN - 1);
        name[MAX_SYMLEN - 1] = '\0';
        char *end = name + strlen(name);
        while (end > name && isspace((unsigned char)end[-1])) *--end = '\0';
        value = ec->next_value;
    }
    if (name[0] == '\0') return; /* trailing comma before '}': "BLUE," then "}" -- nothing to add */
    if (!isalpha((unsigned char)name[0]) && name[0] != '_') {
        char msg[192];
        snprintf(msg, sizeof(msg), "enum '%s': '%s' is not a valid member name", ec->name, name);
        fail(msg);
    }

    char qualified[2 * MAX_SYMLEN];
    snprintf(qualified, sizeof(qualified), "%s.%s", ec->name, name);
    if (find_enum_member(qualified)) failf("enum member '%s' is already defined", qualified);
    DA_ENSURE(enum_members, enum_members_cap, nenum_members, enum_member_t);

    enum_member_t *m = &enum_members[nenum_members++];
    strncpy(m->qualified_name, qualified, sizeof(m->qualified_name) - 1);
    m->qualified_name[sizeof(m->qualified_name) - 1] = '\0';
    m->value = value;

    ec->next_value = value + 1;
}

void struct_finish_field(struct_def_t *sd, const char *field_text) {
    /* field_text: one ';'-terminated piece already stripped of the
       trailing ';', of the form 'iK name' or 'fK name'. */
    char tokbuf[MAX_LINE];
    strncpy(tokbuf, field_text, sizeof(tokbuf) - 1);
    tokbuf[sizeof(tokbuf) - 1] = '\0';
    char *sizetok = strtok(tokbuf, " \t");
    char *nametok = strtok(NULL, " \t");
    if (!sizetok || !nametok) {
        char msg[192];
        snprintf(msg, sizeof(msg), "struct '%s': malformed field (expected 'iK name;' or 'fK name;', got '%s')", sd->name, field_text);
        fail(msg);
    }
    if (strtok(NULL, " \t")) {
        char msg[192];
        snprintf(msg, sizeof(msg), "struct '%s': malformed field '%s' (too many tokens -- no initializers, no arrays, in a struct field)", sd->name, field_text);
        fail(msg);
    }

    int is_float_field = 0;
    if (sizetok[0] == 'f' && isdigit((unsigned char)sizetok[1])) is_float_field = 1;
    else if (sizetok[0] != 'i' || !isdigit((unsigned char)sizetok[1])) {
        char msg[192];
        snprintf(msg, sizeof(msg), "struct '%s': expected a size specifier (i8/i16/i32/i64 or f32/f64) before field name, got '%s'", sd->name, sizetok);
        fail(msg);
    }
    int bits = atoi(sizetok + 1);
    int size = bits / 8;
    if (size != 1 && size != 2 && size != 4 && size != 8) {
        char msg[192];
        snprintf(msg, sizeof(msg), "struct '%s': invalid field width '%s' (use i8/i16/i32/i64 or f32/f64)", sd->name, sizetok);
        fail(msg);
    }
    if (is_float_field && size != 4 && size != 8) {
        char msg[128];
        snprintf(msg, sizeof(msg), "struct '%s': float field width must be f32 or f64", sd->name);
        fail(msg);
    }

    if (find_struct_field(sd, nametok)) {
        char msg[192];
        snprintf(msg, sizeof(msg), "struct '%s': field '%s' is already defined", sd->name, nametok);
        fail(msg);
    }
    DA_ENSURE(sd->fields, sd->fields_cap, sd->nfields, struct_field_t);

    struct_field_t *f = &sd->fields[sd->nfields++];
    strncpy(f->name, nametok, MAX_SYMLEN - 1);
    f->name[MAX_SYMLEN - 1] = '\0';
    f->size_bytes = size;
    f->is_float = is_float_field;
    f->offset = sd->total_size; /* tight-packed: no padding, ever */
    sd->total_size += size;
}

void substitute_all_equs(void) {
    if (nequs == 0) return; /* common case: no equ constants used at all */
    for (int li = 0; li < npp_lines; li++) {
        const char *line = pp_lines[li].text;
        char out[MAX_LINE];
        size_t len = 0;
        const char *p = line;
        while (*p) {
            if (!in_string_at(line, p) && is_ident_char(*p) && (p == line || !is_ident_char(p[-1]))) {
                const char *start = p;
                const char *q = start;
                while (is_ident_char(*q)) q++;
                size_t tok_len = (size_t)(q - start);
                equ_def_t *matched = NULL;
                for (int i = 0; i < nequs; i++) {
                    if (strlen(equs[i].name) == tok_len && strncmp(start, equs[i].name, tok_len) == 0) {
                        matched = &equs[i];
                        break;
                    }
                }
                if (matched) {
                    char valtext[32];
                    snprintf(valtext, sizeof(valtext), "%ld", matched->value);
                    len = buf__append(out, sizeof(out), len, valtext, strlen(valtext));
                } else {
                    len = buf__append(out, sizeof(out), len, start, tok_len);
                }
                p = q;
                continue;
            }
            len = buf__append(out, sizeof(out), len, p, 1);
            p++;
        }
        if (len >= sizeof(out)) len = sizeof(out) - 1;
        out[len] = '\0';
        strncpy(pp_lines[li].text, out, MAX_LINE - 1);
        pp_lines[li].text[MAX_LINE - 1] = '\0';
    }
}

void substitute_all_aliases(void) {
    if (naliases == 0) return; /* common case: no aliases used at all */
    for (int li = 0; li < npp_lines; li++) {
        const char *line = pp_lines[li].text;
        char out[MAX_LINE];
        size_t len = 0;
        const char *p = line;
        while (*p) {
            if (!in_string_at(line, p) && is_ident_char(*p) && (p == line || !is_ident_char(p[-1]))) {
                const char *start = p;
                const char *q = start;
                while (is_ident_char(*q)) q++;
                size_t tok_len = (size_t)(q - start);
                alias_def_t *matched = NULL;
                for (int i = 0; i < naliases; i++) {
                    if (strlen(aliases[i].name) == tok_len && strncmp(start, aliases[i].name, tok_len) == 0) {
                        matched = &aliases[i];
                        break;
                    }
                }
                if (matched) {
                    len = buf__append(out, sizeof(out), len, matched->regtext, strlen(matched->regtext));
                } else {
                    len = buf__append(out, sizeof(out), len, start, tok_len);
                }
                p = q;
                continue;
            }
            len = buf__append(out, sizeof(out), len, p, 1);
            p++;
        }
        if (len >= sizeof(out)) len = sizeof(out) - 1;
        out[len] = '\0';
        strncpy(pp_lines[li].text, out, MAX_LINE - 1);
        pp_lines[li].text[MAX_LINE - 1] = '\0';
    }
}

void substitute_all_enums(void) {
    if (nenum_members == 0) return;
    for (int li = 0; li < npp_lines; li++) {
        const char *line = pp_lines[li].text;
        char out[MAX_LINE];
        size_t len = 0;
        const char *p = line;
        while (*p) {
            if (!in_string_at(line, p) && is_ident_char(*p) && (p == line || !is_ident_char(p[-1]))) {
                const char *start = p;
                const char *q = start;
                while (is_ident_char(*q)) q++;
                /* Only a genuine 'IDENT.IDENT' (dot immediately
                   followed by another ident char) is treated as a
                   possible enum reference -- 'foo.' at end of line, or
                   a field-access-looking-but-unrelated token, just
                   falls through unchanged and is left for whatever
                   parser actually owns that syntax. */
                const char *full_end = q;
                if (*q == '.' && is_ident_char(q[1])) {
                    const char *m_start = q + 1;
                    const char *m_end = m_start;
                    while (is_ident_char(*m_end)) m_end++;
                    full_end = m_end;
                }
                size_t tok_len = (size_t)(full_end - start);
                enum_member_t *matched = NULL;
                if (full_end != q) { /* only look up if we actually saw a dotted form */
                    char qualified[2 * MAX_SYMLEN];
                    size_t qlen = tok_len < sizeof(qualified) - 1 ? tok_len : sizeof(qualified) - 1;
                    memcpy(qualified, start, qlen);
                    qualified[qlen] = '\0';
                    matched = find_enum_member(qualified);
                }
                if (matched) {
                    char valtext[32];
                    snprintf(valtext, sizeof(valtext), "%ld", matched->value);
                    len = buf__append(out, sizeof(out), len, valtext, strlen(valtext));
                    p = full_end;
                } else {
                    /* not a known enum reference -- emit just the first
                       identifier run unchanged (not the dotted tail),
                       so 'p.x' (a struct field access) passes through
                       untouched and gets re-scanned from '.' onward on
                       the next loop iteration */
                    len = buf__append(out, sizeof(out), len, start, (size_t)(q - start));
                    p = q;
                }
                continue;
            }
            len = buf__append(out, sizeof(out), len, p, 1);
            p++;
        }
        if (len >= sizeof(out)) len = sizeof(out) - 1;
        out[len] = '\0';
        strncpy(pp_lines[li].text, out, MAX_LINE - 1);
        pp_lines[li].text[MAX_LINE - 1] = '\0';
    }
}

void substitute_all_sizeofs(void) {
    if (nstruct_defs == 0) return;
    static const char PFX[] = "sizeof(";
    const size_t pfxlen = sizeof(PFX) - 1;
    for (int li = 0; li < npp_lines; li++) {
        const char *line = pp_lines[li].text;
        char out[MAX_LINE];
        size_t len = 0;
        const char *p = line;
        while (*p) {
            if (!in_string_at(line, p) && strncmp(p, PFX, pfxlen) == 0) {
                const char *name_start = p + pfxlen;
                const char *name_end = name_start;
                while (is_ident_char(*name_end)) name_end++;
                if (*name_end == ')' && name_end != name_start) {
                    char name[MAX_SYMLEN];
                    size_t nlen = (size_t)(name_end - name_start);
                    if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
                    memcpy(name, name_start, nlen);
                    name[nlen] = '\0';
                    struct_def_t *sd = find_struct_def(name);
                    if (sd) {
                        char valtext[32];
                        snprintf(valtext, sizeof(valtext), "%d", sd->total_size);
                        len = buf__append(out, sizeof(out), len, valtext, strlen(valtext));
                        p = name_end + 1; /* skip past the ')' too */
                        continue;
                    }
                    /* sizeof(X) where X isn't a known struct: leave
                       entirely untouched and let whatever parses this
                       line report its own, more specific error --
                       'sizeof' is not otherwise a reserved word in
                       Chard, so a token that merely looks like a call
                       to it but isn't a real struct name shouldn't be
                       treated as a hard error here. */
                }
            }
            len = buf__append(out, sizeof(out), len, p, 1);
            p++;
        }
        if (len >= sizeof(out)) len = sizeof(out) - 1;
        out[len] = '\0';
        strncpy(pp_lines[li].text, out, MAX_LINE - 1);
        pp_lines[li].text[MAX_LINE - 1] = '\0';
    }
}

