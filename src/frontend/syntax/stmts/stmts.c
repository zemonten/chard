#include "../../../chard.h"

int split__statements(const char *line, char ***out_chunks) {
    static macro_line_t *scratch = NULL; /* MAX_LINE-sized rows, grown on demand */
    static int scratch_cap = 0;
    static char **chunks = NULL;
    static int chunks_cap = 0;
    int n = 0;
    const char *p = line;

    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;

        const char *start;
        const char *end; /* one past the last character of this chunk */

        if (*p == '}') {
            const char *after = p + 1;
            while (isspace((unsigned char)*after)) after++;
            int is_else = strncmp(after, "else", 4) == 0 &&
                          (after[4] == '\0' || isspace((unsigned char)after[4]) || after[4] == '{');
            const char *brace = is_else ? strchr(after, '{') : NULL;
            if (is_else && brace) {
                start = p;
                end = brace + 1; /* '} else {' as one chunk */
            } else {
                start = p;
                end = p + 1; /* bare '}' as its own chunk */
            }
        } else {
            start = p;

            /* If this chunk opens with '@' (a label decl), skip past
               its name/signature region before scanning for boundary
               characters, so an old-style '@name: {' or
               '@name(a, b) -> r1: {' doesn't have its own ':' or
               ',' mistaken for a statement/operand boundary this loop
               would otherwise split on. New-style '@name {' has no
               such characters in this region at all, so the skip is
               harmless (it just lands right back on '{'). */
            const char *scan_from = start;
            if (*start == '@') {
                const char *sp = start + 1;
                while (*sp && *sp != '{' && *sp != ';' && *sp != '(' && !isspace((unsigned char)*sp)) sp++;
                if (*sp == '(') {
                    int depth = 0;
                    while (*sp) {
                        if (*sp == '(') depth++;
                        else if (*sp == ')') { depth--; sp++; if (depth == 0) break; continue; }
                        sp++;
                    }
                }
                while (*sp == ' ' || *sp == '\t') sp++;
                if (*sp == '-' && sp[1] == '>') {
                    sp += 2;
                    while (*sp == ' ' || *sp == '\t') sp++;
                    while (*sp && *sp != ':' && *sp != '{' && *sp != ';' && !isspace((unsigned char)*sp)) sp++;
                }
                while (*sp == ' ' || *sp == '\t') sp++;
                if (*sp == ':') sp++; /* old-style trailing ':', if present */
                scan_from = sp;
            }

            const char *q = scan_from;
            int in_str = 0;
            while (*q) {
                if (*q == '\\' && in_str && q[1] != '\0') { q += 2; continue; }
                if (*q == '"') { in_str = !in_str; q++; continue; }
                if (!in_str && *q == ';' && q[1] == ';') break; /* ';;' mid-line separator,
                    checked ahead of plain ';' so it isn't mistaken for one statement-end
                    followed by an empty ';'-led chunk */
                if (!in_str && (*q == ';' || *q == '{' || *q == '}' || *q == ':')) break;
                q++;
            }
            if (*q == ';' && q[1] == ';') q += 2;   /* ';;' mid-line separator: consume both, drop them */
            else if (*q == ';' || *q == '{') q++;   /* include the delimiter */
            else if (*q == ':') q++;                /* mid-line separator: consume it, drop it */
            end = (q == start) ? start + 1 : q; /* never spin on an unexpected char */
        }

        DA_ENSURE(scratch, scratch_cap, n, macro_line_t);
        DA_ENSURE(chunks, chunks_cap, n, char *);
        size_t len = (size_t)(end - start);
        /* A mid-line ':' or ';;' separator was matched by scanning from
           scan_from but end/start still span the whole chunk
           (including a leading '@...' region, if any) -- so trimming
           the trailing separator itself (not swallowed into the next
           chunk) only needs to happen when q pointed at one of them
           and got incremented above; do that by shrinking len back
           accordingly in that case. */
        if (len > 1 && start[len - 1] == ';' && start[len - 2] == ';') len -= 2;
        else if (len > 0 && start[len - 1] == ':') len--;
        if (len >= MAX_LINE) len = MAX_LINE - 1;
        memcpy(scratch[n], start, len);
        scratch[n][len] = '\0';
        chunks[n] = scratch[n];
        n++;

        p = end;
    }
    *out_chunks = chunks;
    return n;
}

