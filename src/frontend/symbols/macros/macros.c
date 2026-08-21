#include "../../../chard.h"

macro_def_t *find__macro(const char *name) {
    for (int i = 0; i < nmacros; i++)
        if (strcmp(macros[i].name, name) == 0) return &macros[i];
    return NULL;
}

void expand_macro_line(const char *line, const macro_def_t *m,
                               char *args[MAX_MACRO_PARAMS], int expand_id,
                               char *out, size_t out_cap) {
    size_t len = 0;
    const char *p = line;
    while (*p) {
        if (!in_string_at(line, p) && *p == '@' && is_ident_char(p[1])) {
            const char *start = p + 1;
            const char *q = start;
            while (is_ident_char(*q)) q++;
            char suffix[16];
            snprintf(suffix, sizeof(suffix), "__m%d", expand_id);
            len = buf__append(out, out_cap, len, "@", 1);
            len = buf__append(out, out_cap, len, start, (size_t)(q - start));
            len = buf__append(out, out_cap, len, suffix, strlen(suffix));
            p = q;
            continue;
        }
        if (!in_string_at(line, p) && is_ident_char(*p) && (p == line || !is_ident_char(p[-1]))) {
            const char *start = p;
            const char *q = start;
            while (is_ident_char(*q)) q++;
            size_t tok_len = (size_t)(q - start);
            int matched = -1;
            for (int i = 0; i < m->nparams; i++) {
                if (strlen(m->params[i]) == tok_len && strncmp(start, m->params[i], tok_len) == 0) {
                    matched = i;
                    break;
                }
            }
            if (matched >= 0) {
                len = buf__append(out, out_cap, len, args[matched], strlen(args[matched]));
            } else {
                len = buf__append(out, out_cap, len, start, tok_len);
            }
            p = q;
            continue;
        }
        len = buf__append(out, out_cap, len, p, 1);
        p++;
    }
    if (len >= out_cap) len = out_cap - 1;
    out[len] = '\0';
}

int split_macro_args(char *arglist, char *args[MAX_MACRO_PARAMS]) {
    int n = 0;
    char *p = arglist;
    while (1) {
        while (isspace((unsigned char)*p)) p++;
        char *start = p;
        char *comma = strchr(p, ',');
        char *end = comma ? comma : p + strlen(p);
        char *trimmed_end = end;
        while (trimmed_end > start && isspace((unsigned char)trimmed_end[-1])) trimmed_end--;
        *trimmed_end = '\0';
        if (n >= MAX_MACRO_PARAMS) fail("too many macro arguments");
        args[n++] = start;
        if (!comma) break;
        p = comma + 1;
    }
    if (n == 1 && args[0][0] == '\0') n = 0; /* NAME() with no args at all */
    return n;
}

macro_def_t *match_macro_call(const char *line, const char **args_start, const char **args_end) {
    const char *p = line;
    if (!is_ident_char(*p)) return NULL;
    const char *name_start = p;
    while (is_ident_char(*p)) p++;
    if (*p != '(') return NULL;
    size_t namelen = (size_t)(p - name_start);
    if (namelen >= MAX_SYMLEN) return NULL;
    char name[MAX_SYMLEN];
    memcpy(name, name_start, namelen);
    name[namelen] = '\0';
    macro_def_t *m = find__macro(name);
    if (!m) return NULL;
    const char *close = strchr(p, ')');
    if (!close) return NULL;
    *args_start = p + 1;
    *args_end = close;
    return m;
}

void expand_all_macro_calls(void) {
    for (int pass = 0; pass < MAX_MACRO_EXPAND_DEPTH; pass++) {
        int any_expanded = 0;
        pp_line_t *next = NULL;
        int nnext = 0;
        int next_cap = 0;

        for (int i = 0; i < npp_lines; i++) {
            const char *line = pp_lines[i].text;
            const char *args_start, *args_end;
            macro_def_t *m = match_macro_call(line, &args_start, &args_end);

            /* Only treat this as a call if, after the ')', the rest of
               the line is just an optional ';' and whitespace -- so a
               macro name that happens to prefix a longer, unrelated
               statement is never misfired on. */
            if (m) {
                const char *after = args_end + 1;
                while (isspace((unsigned char)*after)) after++;
                if (*after == ';') after++;
                while (isspace((unsigned char)*after)) after++;
                if (*after != '\0') m = NULL;
            }

            if (!m) {
                DA_ENSURE(next, next_cap, nnext, pp_line_t);
                next[nnext++] = pp_lines[i];
                continue;
            }

            any_expanded = 1;
            char arglist[MAX_LINE];
            size_t alen = (size_t)(args_end - args_start);
            if (alen >= sizeof(arglist)) alen = sizeof(arglist) - 1;
            memcpy(arglist, args_start, alen);
            arglist[alen] = '\0';

            char *args[MAX_MACRO_PARAMS];
            int nargs = split_macro_args(arglist, args);
            if (nargs != m->nparams) {
                char msg[128];
                snprintf(msg, sizeof(msg), "macro '%s' expects %d argument(s), got %d",
                         m->name, m->nparams, nargs);
                fail(msg);
            }

            int expand_id = g_macro_expand_counter++;
            for (int b = 0; b < m->nbody; b++) {
                char expanded[MAX_LINE];
                expand_macro_line(m->body[b], m, args, expand_id, expanded, sizeof(expanded));
                DA_ENSURE(next, next_cap, nnext, pp_line_t);
                strncpy(next[nnext].text, expanded, MAX_LINE - 1);
                next[nnext].text[MAX_LINE - 1] = '\0';
                next[nnext].orig_line_no = pp_lines[i].orig_line_no;
                next[nnext].filename = pp_lines[i].filename; /* expanded body
                    lines report the call site's file, same as they already
                    report the call site's line number */
                nnext++;
            }
        }

        free(pp_lines);
        pp_lines = next;
        pp_lines_cap = next_cap;
        npp_lines = nnext;
        if (!any_expanded) return;
    }
    fail("macro expansion did not terminate (macro calling itself recursively?)");
}

