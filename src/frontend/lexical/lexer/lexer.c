#include "../../../chard.h"

int tokenize(char *line, char *tokens[MAX_TOKENS]) {
    int n = 0;
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (n >= MAX_TOKENS) break;

        if (*p == '(') {
            /* Find the matching close paren first (without mutating
               anything), so we know up front whether this is a real
               balanced group or just a stray '('. */
            char *scan = p;
            int depth = 0;
            while (*scan) {
                if (*scan == '(') depth++;
                else if (*scan == ')') { depth--; if (depth == 0) break; }
                scan++;
            }
            if (depth == 0 && *scan == ')') {
                char *close = scan; /* points at the matching ')' */
                /* Compact [p, close] in place, dropping interior
                   whitespace, writing the result starting at p. This can
                   only shrink the span (whitespace removal), so it never
                   overwrites unprocessed text beyond 'close'. */
                char *src = p, *w = p;
                while (src <= close) {
                    if (*src == ' ' || *src == '\t') { src++; continue; }
                    *w++ = *src++;
                }
                /* w now points just past the compacted ')'. Shift
                   whatever follows 'close' in the original buffer down
                   to sit right after the compacted token, so the rest of
                   the line is contiguous again, then terminate the
                   token. */
                size_t taillen = strlen(close + 1);
                memmove(w + 1, close + 1, taillen + 1); /* +1 to also move the NUL */
                tokens[n++] = p;
                *w = '\0';
                p = w + 1;
                continue;
            }
            /* Unbalanced -- fall through and treat '(' as an ordinary
               character; normal whitespace tokenizing below will hand it
               back as (part of) a plain token, and the statement parser
               that eventually looks at it reports a clearer,
               context-specific error than failing right here. */
        }

        char *tok_start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
        tokens[n++] = tok_start;
    }
    return n;
}

