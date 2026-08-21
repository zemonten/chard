#include "../../chard.h"

const char *intern_filename(const char *path) {
    for (int i = 0; i < nfile_names; i++)
        if (strcmp(file_names[i], path) == 0) return file_names[i];
    DA_ENSURE(file_names, file_names_cap, nfile_names, char *);
    char *copy = malloc(strlen(path) + 1);
    if (!copy) { perror("malloc"); exit(1); }
    strcpy(copy, path);
    file_names[nfile_names] = copy;
    return file_names[nfile_names++];
}

int already_included(const char *resolved) {
    for (int i = 0; i < nincluded_paths; i++)
        if (strcmp(included_paths[i], resolved) == 0) return 1;
    return 0;
}

void mark_included(const char *resolved) {
    DA_ENSURE(included_paths, included_paths_cap, nincluded_paths, const char *);
    included_paths[nincluded_paths++] = intern_filename(resolved);
}

void include_push(const char *resolved) {
    if (ninclude_stack >= MAX_INCLUDE_DEPTH)
        failf("'| include'/'| data' nested too deep (possible include cycle involving '%s')", resolved);
    include_stack[ninclude_stack++] = resolved;
}

void include_pop(void) {
    ninclude_stack--;
}

void dirname_of(const char *path, char *out, size_t outsz) {
    const char *slash = strrchr(path, '/');
    if (!slash) { out[0] = '\0'; return; }
    size_t len = (size_t)(slash - path) + 1; /* include the '/' itself */
    if (len >= outsz) len = outsz - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

int resolve_include_path(const char *requested, const char *from_file, char *out, size_t outsz) {
    char dir[MAX_PATH_LEN];
    dirname_of(from_file, dir, sizeof(dir));

    char candidate[MAX_PATH_LEN];
    snprintf(candidate, sizeof(candidate), "%s%s", dir, requested);
    FILE *probe = fopen(candidate, "r");
    if (probe) {
        fclose(probe);
        strncpy(out, candidate, outsz - 1);
        out[outsz - 1] = '\0';
        return 1;
    }

    /* Fall back to the path exactly as written (cwd-relative or
       absolute) -- only worth trying if it differs from the first
       candidate (dir == "" makes them identical, so this avoids
       needlessly opening the same nonexistent path twice). */
    if (dir[0] != '\0') {
        probe = fopen(requested, "r");
        if (probe) {
            fclose(probe);
            strncpy(out, requested, outsz - 1);
            out[outsz - 1] = '\0';
            return 1;
        }
    }

    return 0;
}

void parse_include_operand(const char *directive_name, const char *t, size_t kwlen, char *out, size_t outsz) {
    const char *rest = t + kwlen;
    while (isspace((unsigned char)*rest)) rest++;
    if (*rest != '"') {
        char msg[96];
        snprintf(msg, sizeof(msg), "malformed '%s': expected '%s \"...\"'", directive_name, directive_name);
        fail(msg);
    }
    rest++;
    const char *close = strchr(rest, '"');
    if (!close) failf("unterminated string in '%s' argument", directive_name);
    size_t len = (size_t)(close - rest);
    if (len == 0) failf("'%s': empty argument", directive_name);
    if (len >= outsz) failf("'%s': argument too long", directive_name);
    memcpy(out, rest, len);
    out[len] = '\0';

    const char *after = close + 1;
    while (isspace((unsigned char)*after)) after++;
    if (*after == ';') after++;
    while (isspace((unsigned char)*after)) after++;
    if (*after != '\0')
        failf("unexpected text after '%s \"...\"'", directive_name);
}

void check_include_extension(const char *directive_name, const char *resolved, int is_data) {
    int ok = is_data ? has_extension(resolved, ".chd")
                      : (has_extension(resolved, ".ch") || has_extension(resolved, ".chh"));
    if (ok) return;
    fprintf(stderr,
        "%s:%d: warning: '%s \"%s\"' -- expected a '%s' file by convention (this is not an error, just a naming hint)\n",
        g_filename, g_line_no, directive_name, resolved, is_data ? ".chd" : ".ch'/'.chh");
}

void do_include_directive(const char *directive_name, size_t keyword_len, const char *t) {
    char requested[MAX_PATH_LEN];
    parse_include_operand(directive_name, t, keyword_len, requested, sizeof(requested));

    char resolved[MAX_PATH_LEN];
    if (!resolve_include_path(requested, g_filename, resolved, sizeof(resolved))) {
        failf("cannot open '%s' (from '| include'/'| data')", requested);
        return;
    }

    check_include_extension(directive_name, resolved, strcmp(directive_name, "| data") == 0 /* "| data" */);

    /* Cycle check must run before the pragma-once check: a real cycle
       (A includes B includes A) means A is already marked included by
       the time control loops back around to it, which is exactly what
       pragma-once alone would otherwise interpret as "already handled,
       nothing to do" -- silently dropping the second occurrence instead
       of reporting the cycle. Checking the still-open include stack
       first tells the two cases apart: still on the stack means it's a
       cycle (this file is its own ancestor), already-included-but-not-
       on-the-stack means it's an ordinary diamond include. */
    for (int i = 0; i < ninclude_stack; i++) {
        if (strcmp(include_stack[i], resolved) == 0)
            failf("include cycle detected: '%s' includes itself (directly or indirectly)", resolved);
    }

    if (already_included(resolved)) return; /* pragma-once */

    FILE *inc = fopen(resolved, "r");
    if (!inc) { failf("cannot open '%s' (from '| include'/'| data')", resolved); return; }

    mark_included(resolved);
    const char *interned = intern_filename(resolved);
    include_push(interned);

    const char *saved_filename = g_filename;
    int saved_line_no = g_line_no;
    g_filename = interned;
    g_line_no = 0;

    collect_source_and_macro_defs(inc);

    fclose(inc);
    g_filename = saved_filename;
    g_line_no = saved_line_no;
    include_pop();
}

