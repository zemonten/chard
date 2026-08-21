#include "../../../chard.h"

int parse__decl(char *tokens[], int ntok, char *raw_line) {
    if (ntok < 3) return 0;

    section_t sec;
    int is_local = 0;
    int is_data_kw = 0;
    if (strcmp(tokens[0], "volatile") == 0) sec = SEC_DATA;
    else if (strcmp(tokens[0], "bss") == 0) sec = SEC_BSS;
    else if (strcmp(tokens[0], "local") == 0) { sec = SEC_LOCAL; is_local = 1; }
    else if (strcmp(tokens[0], "data") == 0) { sec = SEC_DATA; is_data_kw = 1; }
    /* 'rodata' -- an initialized, read-only global: same grammar and
       storage shape as 'volatile' (scalar/ascii; array form is still
       'data iK name[] = ...;', not duplicated here -- see that
       branch's own comment for why the initialized-array case has its
       own distinct '[]' syntax rather than living on every SEC_DATA-ish
       keyword), just placed in '.rodata' instead of '.data' at emit
       time (each backend's SEC_RODATA loop, right after its SEC_DATA
       one) so it lands in a linker-enforced read-only page rather than
       a writable one. Chard has no notion of "this write is illegal"
       at compile time -- nothing here rejects a later 'storeN val >
       name;' against a 'rodata' symbol -- the read-only guarantee is
       entirely the linker/loader's (a write to '.rodata' faults at
       runtime, same as it would for a 'const' global compiled by any
       other toolchain); 'rodata' exists so a program can say "this data
       is meant to be immutable" and get that enforced, not to add a
       new class of compile-time check. Available in both '| mode elf;'
       and '| mode bare;' (unlike the kernel-syscall-wrapper builtins),
       since read-only global data is a linker/section concept, not a
       kernel-facing one -- and it's exactly the kind of thing BARE mode
       most wants (a GDT, a font table, a constant lookup table baked
       into a freestanding image). */
    else if (strcmp(tokens[0], "rodata") == 0) sec = SEC_RODATA;
    else return 0;

    /* data iK name[] = v1, v2, ...; -- a named, initialized list of
       values in SEC_DATA: assembly's '.byte 1,2,3,4'/'dw 1,2,3,4'
       equivalent, letting a lookup/jump table be written as one line
       instead of one 'volatile iK name = v;' per element. The brace
       list contains commas, so (like ascii's quoted string) it can't
       be parsed from the whitespace-split token array alone the way a
       scalar initializer's single value can -- but unlike ascii's
       quoted string, this list must NOT be brace-delimited: braces are
       one of split__statements' own statement-boundary characters (see
       its comment), so a literal '{' this early in the pipeline would
       already have been sliced apart into its own chunk before
       parse__decl ever saw it, silently truncating the initializer.
       Comma is not special to split__statements, so the list is instead
       just comma-separated values straight after '=', ended by the
       statement's own trailing ';' -- 'data i32 name[] = 1, 2, 3;'.
       'data fK name[] = v1.v, v2.v, ...;' is the float-element sibling
       of the same form: the size specifier's letter (checked below)
       picks integer vs. float exactly the way it does for a scalar
       'volatile'/'bss'/'local' decl (see is_float_decl further down in
       this function), and the initializer values are then parsed and
       stored as floats instead of integers -- mirroring how a scalar
       float decl's value lives in init_fvalue instead of init_value.
       No 'bss'/'local' data arrays -- an array with no initializer is
       exactly what 'bss iK name[N];'... except that doesn't exist
       either (Chard's only uninitialized-array form today is a
       stack-local array; see declare_local_array), so 'data'
       intentionally covers only the initialized/global case, not
       every combination the keyword grid might suggest. */
    if (is_data_kw) {
        if (ntok < 2) fail("expected size specifier after 'data' (e.g. data i32 name[] = 1, 2, 3;)");
        const char *szspec = resolve_size_alias(tokens[1]);
        int data_is_float = 0;
        if (szspec[0] == 'f' && isdigit((unsigned char)szspec[1])) {
            data_is_float = 1;
            g_uses_float = 1;
        } else if (szspec[0] != 'i' || !isdigit((unsigned char)szspec[1])) {
            fail("'data' requires a size specifier (e.g. i8/char, i32/int, f32, f64)");
        }
        int dsize = atoi(szspec + 1) / 8;
        if (dsize <= 0) dsize = 1;
        if (data_is_float && dsize != 4 && dsize != 8)
            fail("'data' float size specifier must be f32 or f64");

        if (ntok < 3) fail("expected identifier after 'data iK'");
        char name[MAX_SYMLEN];
        strncpy(name, tokens[2], MAX_SYMLEN - 1);
        name[MAX_SYMLEN - 1] = '\0';
        char *bracket_in_name = strchr(name, '[');
        if (bracket_in_name) *bracket_in_name = '\0';
        int ni = (int)strlen(name);
        if (ni == 0) fail("expected identifier after 'data iK'");
        if (find__decl(name)) failf("redeclaration of '%s'", name);

        if (!strstr(raw_line, "[]")) fail("'data' declaration requires '[]' (e.g. data i32 name[] = 1, 2, 3;)");
        const char *eq = strchr(raw_line, '=');
        if (!eq) fail("'data' declaration requires an initializer list: data iK name[] = v1, v2, ...;");

        char listbuf[MAX_LINE];
        strncpy(listbuf, eq + 1, MAX_LINE - 1);
        listbuf[MAX_LINE - 1] = '\0';
        char *semi = strchr(listbuf, ';');
        if (semi) *semi = '\0';

        long *vals = NULL; double *fvals = NULL;
        int vals_cap = 0, fvals_cap = 0;
        /* Jump-table support: an element written as '&label' instead of
           a numeric literal records the address of a code label rather
           than a plain integer. is_label/val_labels are separate arrays
           grown in step with vals (same nvals index), each with its OWN
           capacity variable -- DA_ENSURE mutates whatever cap variable
           it's given, so three arrays sharing one cap variable would
           silently under-grow the second and third once the first call
           had already bumped the shared cap past nvals. Left NULL/0
           whenever the array turns out to be all-numeric (any_label
           stays 0), so an ordinary 'data' array pays nothing extra. */
        int *is_label = NULL; char **val_labels = NULL;
        int is_label_cap = 0, val_labels_cap = 0;
        int any_label = 0;
        int nvals = 0;
        char *tok = strtok(listbuf, ",");
        while (tok) {
            char *vt = trim(tok);
            if (*vt != '\0') {
                /* '&label' -- a jump-table entry: this element is the
                   address of a code label, not a numeric literal. Only
                   meaningful for an integer element (a float element
                   can't hold a code address, so this is rejected the
                   same way a malformed numeric literal would be).
                   Label text is trusted as-is and emitted verbatim,
                   exactly like OP_JMP's dst.sym -- Chard doesn't
                   validate 'jmp label;' targets at compile time either
                   (labels are resolved by the assembler), so requiring
                   the label to already exist here would be a stricter
                   check than any other reference to a label gets. */
                if (*vt == '&') {
                    char *lbl = trim(vt + 1);
                    if (*lbl == '\0') failf("'data %s': '&' must be followed by a label name (got bare '&')", name);
                    if (data_is_float) fail_fmt("'data %s': '&%s' (a label address) cannot be an 'fK' element -- jump-table entries must be an integer size (i64 for a full address)", name, lbl);
                    DA_ENSURE(vals, vals_cap, nvals, long);
                    DA_ENSURE(is_label, is_label_cap, nvals, int);
                    DA_ENSURE(val_labels, val_labels_cap, nvals, char *);
                    vals[nvals] = 0;
                    is_label[nvals] = 1;
                    val_labels[nvals] = str_dup(lbl);
                    any_label = 1;
                    nvals++;
                    tok = strtok(NULL, ",");
                    continue;
                }
                if (data_is_float) {
                    if (!is_float_literal(vt)) {
                        char msg[192];
                        snprintf(msg, sizeof(msg), "'data %s': initializer values must be plain float literals (got '%s')", name, vt);
                        fail(msg);
                    }
                } else if (!is__number(vt)) {
                    char msg[192];
                    snprintf(msg, sizeof(msg), "'data %s': initializer values must be plain integer literals or '&label' jump-table entries (got '%s')", name, vt);
                    fail(msg);
                }
                if (data_is_float) { DA_ENSURE(fvals, fvals_cap, nvals, double); }
                else {
                    DA_ENSURE(vals, vals_cap, nvals, long);
                    DA_ENSURE(is_label, is_label_cap, nvals, int);
                    DA_ENSURE(val_labels, val_labels_cap, nvals, char *);
                }
                if (data_is_float) fvals[nvals] = atof(vt);
                else { vals[nvals] = parse__number(vt); is_label[nvals] = 0; val_labels[nvals] = NULL; }
                nvals++;
            }
            tok = strtok(NULL, ",");
        }
        if (nvals == 0) fail("'data' initializer list must contain at least one value");

        DA_ENSURE(decls, decls_cap, ndecls, decl_t);
        decl_t *d = &decls[ndecls++];
        memset(d, 0, sizeof(*d));
        strncpy(d->name, name, MAX_SYMLEN - 1);
        d->section = SEC_DATA;
        d->size_bytes = dsize;
        d->is_data_array = 1;
        d->is_float = data_is_float;
        d->has_init = 1;
        d->array_len = nvals;
        if (data_is_float) { d->data_fvals = fvals; d->data_fvals_cap = fvals_cap; free(vals); free(is_label); free(val_labels); }
        else {
            d->data_vals = vals; d->data_vals_cap = vals_cap;
            if (any_label) { d->data_val_is_label = is_label; d->data_val_labels = val_labels; }
            else { free(is_label); free(val_labels); }
            free(fvals);
        }
        return 1;
    }


    /* ascii declarations contain a quoted string with spaces, so they
       can't be parsed from the whitespace-split token array -- work
       from the raw line instead. Valid in 'volatile' (initialized),
       'bss' (uninitialized, fixed-capacity buffer), and 'local'
       (uninitialized, fixed-capacity, stack-resident) sections.

       'bss ascii NAME[N];' reserves N uninitialized bytes (a fixed-
       capacity buffer, e.g. for a 'read()' destination) instead of a
       quoted literal -- there is no string to parse at compile time,
       so this is checked and handled first, ahead of the
       q1/find_string_close_quote logic below which assumes a literal
       is present. Its companion 'NAME_len' is likewise emitted
       uninitialized into .bss (see each backend's SEC_BSS loop) rather
       than pre-set to N or to str_len (there is no known string
       length yet) -- since .bss is zero-filled by the OS at process
       start, NAME_len reads as 0 until the program stores an actual
       runtime length into it (e.g. after a 'read()' into NAME),
       matching out()'s existing contract of writing exactly
       '[NAME_len]' bytes from NAME.

       'local ascii NAME[N];' is the stack-resident twin of 'bss
       ascii': same fixed-capacity, no-initializer, "reads as garbage
       until something writes it" shape, just laid out below fp
       instead of in .bss. Handled in its own branch below (it needs
       declare_local_array + a companion local for NAME_len, not the
       .bss reservation this branch does), but checked here first for
       the same reason 'bss ascii' is checked ahead of the literal-
       parsing logic: it has a '[N]' capacity, not a quoted string, so
       it can't fall through into the q1/find_string_close_quote path
       below. */
    if (strcmp(tokens[1], "ascii") == 0 && sec == SEC_BSS) {
        if (ntok < 3) fail("expected identifier after 'bss ascii'");
        char name[MAX_SYMLEN];
        strncpy(name, tokens[2], MAX_SYMLEN - 1);
        name[MAX_SYMLEN - 1] = '\0';
        char *bracket_in_name = strchr(name, '[');
        if (!bracket_in_name) fail("'bss ascii' requires a capacity (e.g. bss ascii name[64]; -- bss/local ascii with no size is not supported)");
        *bracket_in_name = '\0';
        int ni = (int)strlen(name);
        if (ni == 0) fail("expected identifier after 'bss ascii'");
        if (find__decl(name)) failf("redeclaration of '%s'", name);

        char *close_bracket = strchr(bracket_in_name + 1, ']');
        if (!close_bracket) fail("malformed 'bss ascii' declaration: missing ']' (bss ascii name[N];)");
        char numbuf[32];
        size_t numlen = (size_t)(close_bracket - (bracket_in_name + 1));
        if (numlen == 0 || numlen >= sizeof(numbuf)) fail("array size must be a positive integer literal (bss ascii name[N];)");
        strncpy(numbuf, bracket_in_name + 1, numlen);
        numbuf[numlen] = '\0';
        for (size_t ci = 0; ci < numlen; ci++)
            if (!isdigit((unsigned char)numbuf[ci])) fail("array size must be a positive integer literal (bss ascii name[N];)");
        int cap = atoi(numbuf);
        if (cap <= 0) fail("'bss ascii' capacity must be a positive integer (bss ascii name[N];)");

        if (strchr(raw_line, '=')) {
            char msg[256];
            snprintf(msg, sizeof(msg), "'bss %s[%d]': 'bss ascii' cannot have an initializer (bss is always uninitialized -- did you mean 'volatile ascii'?)", name, cap);
            fail(msg);
        }

        DA_ENSURE(decls, decls_cap, ndecls, decl_t);
        decl_t *d = &decls[ndecls++];
        memset(d, 0, sizeof(*d));
        strncpy(d->name, name, MAX_SYMLEN - 1);
        d->section = SEC_BSS;
        d->is_ascii = 1;
        d->size_bytes = 1;
        d->array_len = cap;
        return 1;
    }
    /* 'local ascii NAME[N];' -- same capacity-in-brackets shape as
       'bss ascii', reusing that branch's bracket/digit-parsing logic
       (kept identical on purpose: no reason for the two to accept
       differently-shaped capacities), but reserving the bytes on the
       stack via declare_local_array instead of in .bss, and giving
       NAME_len its own ordinary local slot (an i64, via declare__local)
       instead of a linker symbol -- there is no assembler-visible
       'NAME_len' name for a stack slot the way there is for a .bss
       symbol, so the length has to be a second real decl the parser
       tracks, not just a naming convention applied at emission time.
       This also means NAME_len is directly readable/writable like any
       other local (e.g. 'mv NAME_len > r1;'), which is a strict
       superset of what the global form offers. Like 'bss ascii',
       NAME_len starts as garbage, not 0 -- a fresh stack slot has no
       zero-fill guarantee the way .bss does, so a program should
       write it explicitly (e.g. after a 'read()' into NAME) before
       relying on it; this is the same "uninitialized until written"
       contract every other local already has, not a new one. */
    if (strcmp(tokens[1], "ascii") == 0 && is_local) {
        if (ntok < 3) fail("expected identifier after 'local ascii'");
        char name[MAX_SYMLEN];
        strncpy(name, tokens[2], MAX_SYMLEN - 1);
        name[MAX_SYMLEN - 1] = '\0';
        char *bracket_in_name = strchr(name, '[');
        if (!bracket_in_name) fail("'local ascii' requires a capacity (e.g. local ascii name[64]; -- bss/local ascii with no size is not supported)");
        *bracket_in_name = '\0';
        int ni = (int)strlen(name);
        if (ni == 0) fail("expected identifier after 'local ascii'");
        if (find__decl(name)) failf("redeclaration of '%s'", name);

        char *close_bracket = strchr(bracket_in_name + 1, ']');
        if (!close_bracket) fail("malformed 'local ascii' declaration: missing ']' (local ascii name[N];)");
        char numbuf[32];
        size_t numlen = (size_t)(close_bracket - (bracket_in_name + 1));
        if (numlen == 0 || numlen >= sizeof(numbuf)) fail("array size must be a positive integer literal (local ascii name[N];)");
        strncpy(numbuf, bracket_in_name + 1, numlen);
        numbuf[numlen] = '\0';
        for (size_t ci = 0; ci < numlen; ci++)
            if (!isdigit((unsigned char)numbuf[ci])) fail("array size must be a positive integer literal (local ascii name[N];)");
        int cap = atoi(numbuf);
        if (cap <= 0) fail("'local ascii' capacity must be a positive integer (local ascii name[N];)");

        if (strchr(raw_line, '=')) {
            char msg[256];
            snprintf(msg, sizeof(msg), "'local ascii %s[%d]': 'local ascii' cannot have an initializer (a stack slot has nothing to pre-fill it from -- did you mean 'volatile ascii'?)", name, cap);
            fail(msg);
        }

        if (strlen(name) + 4 >= MAX_SYMLEN) /* +4 for "_len", +1 implicit for the NUL already covered by ">=" */
            failf("'local ascii' name '%s' too long once '_len' is appended", name);
        /* Built via strncpy+strcat rather than snprintf("%s_len", ...):
           functionally identical, but doesn't trip -Wformat-truncation
           (which can't see the length check just above already proves
           this can't truncate). */
        char lenname[MAX_SYMLEN];
        strncpy(lenname, name, MAX_SYMLEN - 1);
        lenname[MAX_SYMLEN - 1] = '\0';
        strncat(lenname, "_len", MAX_SYMLEN - 1 - strlen(lenname));
        if (find__decl(lenname)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "redeclaration of '%s' (needed internally as '%s's length slot)", lenname, name);
            fail(msg);
        }

        decl_t *ad = declare_local_array(name, 1, cap);
        ad->is_ascii = 1;
        /* declare__local may realloc decls[] out from under `ad` (it's
           an index into the same growable array, not a stable
           pointer) -- reread the array/its size via array_len/
           size_bytes only, never dereference `ad` again after this
           call. Nothing below does. */
        declare__local(lenname, 8);
        return 1;
    }
    if (strcmp(tokens[1], "ascii") == 0) {
        if (sec != SEC_DATA && sec != SEC_RODATA) fail("'ascii' requires 'volatile', 'rodata', or 'bss' (use 'local ascii' inside a block for a stack-resident buffer)");
        const char *q1 = strchr(raw_line, '"');
        if (!q1) fail("'ascii' declaration requires a quoted string literal");
        const char *q2 = find_string_close_quote(q1);
        if (!q2) fail("unterminated string literal");

        /* name is tokens[2] ('ascii' NAME = "..."), possibly with a
           trailing '=' glued on if there was no space before it */
        if (ntok < 3) fail("expected identifier after 'ascii'");
        char name[MAX_SYMLEN];
        strncpy(name, tokens[2], MAX_SYMLEN - 1);
        name[MAX_SYMLEN - 1] = '\0';
        char *eq_in_name = strchr(name, '=');
        if (eq_in_name) *eq_in_name = '\0';
        int ni = (int)strlen(name);
        if (ni == 0) fail("expected identifier after 'ascii'");

        /* resolve a small set of escapes (\n, \t, \\, \") while copying */
        char resolved[MAX_STRLEN];
        int ri = 0;
        for (const char *p = q1 + 1; p < q2 && ri < MAX_STRLEN - 1; p++) {
            if (*p == '\\' && p + 1 < q2) {
                p++;
                char c = *p == 'n' ? '\n' : *p == 't' ? '\t' :
                         *p == '\\' ? '\\' : *p == '"' ? '"' : *p;
                resolved[ri++] = c;
            } else {
                resolved[ri++] = *p;
            }
        }
        resolved[ri] = '\0';

        DA_ENSURE(decls, decls_cap, ndecls, decl_t);
        decl_t *d = &decls[ndecls++];
        memset(d, 0, sizeof(*d));
        strncpy(d->name, name, MAX_SYMLEN - 1);
        d->section = sec;
        d->is_ascii = 1;
        d->has_init = 1;
        memcpy(d->str_val, resolved, ri + 1);
        d->str_len = ri;
        return 1;
    }

    /* 'local StructName name;' -- a stack instance of a previously
       declared struct type. Checked ahead of the iK/fK size-specifier
       branch below since tokens[1] here is a type name, not a size
       specifier -- 'struct' instances are 'local'-only for the same
       reason arrays are (see declare_local_array's comment: no
       linker-backed .data equivalent to pre-fill on the stack, so
       there's nothing for 'volatile'/'bss' struct instances to mean
       here in v1). No initializer syntax either -- same v1 scope limit
       plain local arrays already have. */
    {
        struct_def_t *sd = find_struct_def(tokens[1]);
        if (sd) {
            if (!is_local) failf("struct instances are only supported for 'local' (not 'volatile'/'bss'): '%s'", sd->name);
            char name[MAX_SYMLEN];
            strncpy(name, tokens[2], MAX_SYMLEN - 1);
            name[MAX_SYMLEN - 1] = '\0';

            /* local StructName name[N]; -- an array of struct instances,
               laid out as N copies of sd->total_size bytes back to back
               (same base_offset/element convention declare_local_array
               already uses, just with elem_size == sd->total_size instead
               of a scalar width). No initializer form for the array case
               in v1 -- only the single-instance form below accepts '='. */
            char *bracket = strchr(name, '[');
            if (bracket) {
                char *close_bracket = strchr(bracket, ']');
                if (!close_bracket) fail("malformed array declaration: missing ']' (local StructName name[N];)");
                *bracket = '\0';
                char numbuf[32];
                size_t numlen = (size_t)(close_bracket - bracket - 1);
                if (numlen == 0 || numlen >= sizeof(numbuf)) fail("malformed array declaration: expected an integer inside '[...]'");
                memcpy(numbuf, bracket + 1, numlen);
                numbuf[numlen] = '\0';
                for (size_t ci = 0; ci < numlen; ci++)
                    if (!isdigit((unsigned char)numbuf[ci])) fail("array size must be a positive integer literal (local StructName name[N];)");
                int alen = atoi(numbuf);
                if (alen <= 0) fail("array size must be a positive integer (local StructName name[N];)");
                if (strchr(raw_line, '=')) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "struct array '%s' cannot have an initializer in v1 (local %s %s[%d];)", name, sd->name, name, alen);
                    fail(msg);
                }
                if (name[0] == '\0') failf("expected identifier after 'local %s'", sd->name);
                if (sd->nfields == 0) failf("struct '%s' has no fields (an empty struct cannot be instantiated)", sd->name);

                decl_t *d = declare_local_array(name, sd->total_size, alen);
                strncpy(d->struct_type_name, sd->name, MAX_SYMLEN - 1);
                d->struct_type_name[MAX_SYMLEN - 1] = '\0';
                return 1;
            }

            char *semi = strchr(name, ';');
            if (semi) *semi = '\0';
            char *eq_in_name = strchr(name, '=');
            if (eq_in_name) *eq_in_name = '\0';
            if (name[0] == '\0') failf("expected identifier after 'local %s'", sd->name);

            /* local StructName name = { field: v, field: v, ... };
               Each listed field desugars to the same seed-register +
               store pair sfieldN itself builds (OP_MOV/OP_FMOV into the
               scratch register, then OP_LASTORE/OP_FSTORE at
               base_offset - field->offset) -- so this is pure sugar over
               'local StructName name;' followed by one 'sfieldN v >
               name.field;' per listed field, not a new storage form.
               Fields not listed are left uninitialized, same as a local
               array's short initializer list leaves trailing elements
               uninitialized (see the 'local iK name[N] = ...;' comment
               above). Braces are a split__statements boundary character
               (see the 'data' array comment above for why), so the list
               has to be pulled from raw_line the same way, between the
               '{' and the ';' that follows the matching '}'. */
            const char *eq = strchr(raw_line, '=');
            decl_t *d = declare_local_struct(name, sd);
            if (!eq) return 1; /* plain 'local StructName name;' */

            const char *brace_open = strchr(eq, '{');
            if (!brace_open) fail("malformed struct initializer: expected 'local StructName name = { field: v, ... };'");
            const char *brace_close = strchr(brace_open, '}');
            if (!brace_close) fail("malformed struct initializer: missing '}'");

            char listbuf[MAX_LINE];
            size_t listlen = (size_t)(brace_close - brace_open - 1);
            if (listlen >= sizeof(listbuf)) listlen = sizeof(listbuf) - 1;
            memcpy(listbuf, brace_open + 1, listlen);
            listbuf[listlen] = '\0';

            operand_t instop;
            parse__operand(name, &instop);

            char *tok = strtok(listbuf, ",");
            while (tok) {
                char *pair = trim(tok);
                if (*pair != '\0') {
                    char *colon = strchr(pair, ':');
                    if (!colon) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "struct '%s' initializer: expected 'field: value' (got '%s')", sd->name, pair);
                        fail(msg);
                    }
                    *colon = '\0';
                    char *fieldname = trim(pair);
                    char *valtxt = trim(colon + 1);
                    struct_field_t *fld = find_struct_field(sd, fieldname);
                    if (!fld) {
                        char msg[256];
                        snprintf(msg, sizeof(msg), "struct '%s' initializer: no such field '%s'", sd->name, fieldname);
                        fail(msg);
                    }

                    operand_t fieldop = instop;
                    fieldop.local_offset -= fld->offset;
                    fieldop.local_size = fld->size_bytes;

                    if (fld->is_float) {
                        if (!is_float_literal(valtxt)) {
                            char msg[256];
                            snprintf(msg, sizeof(msg), "struct '%s' initializer: field '%s' needs a plain float literal (got '%s')", sd->name, fieldname, valtxt);
                            fail(msg);
                        }
                        instr_t seed; memset(&seed, 0, sizeof(seed));
                        seed.op = OP_FMOV;
                        seed.src.kind = OPND_IMM;
                        seed.src.is_float = 1;
                        seed.src.fimm = atof(valtxt);
                        seed.dst.kind = OPND_REG;
                        seed.dst.is_float = 1;
                        seed.dst.reg_num = g_finit_scratch_reg;
                        push__instr(seed);

                        instr_t store; memset(&store, 0, sizeof(store));
                        store.op = OP_FSTORE;
                        store.src = seed.dst;
                        store.dst = fieldop;
                        push__instr(store);
                    } else {
                        if (!is__number(valtxt)) {
                            char msg[256];
                            snprintf(msg, sizeof(msg), "struct '%s' initializer: field '%s' needs a plain integer literal (got '%s')", sd->name, fieldname, valtxt);
                            fail(msg);
                        }
                        instr_t seed; memset(&seed, 0, sizeof(seed));
                        seed.op = OP_MOV;
                        seed.src.kind = OPND_IMM;
                        seed.src.imm = parse__number(valtxt);
                        seed.dst.kind = OPND_REG;
                        seed.dst.reg_num = g_init_scratch_reg;
                        push__instr(seed);

                        instr_t store; memset(&store, 0, sizeof(store));
                        store.op = OP_LASTORE;
                        store.elem_size = fld->size_bytes;
                        store.dst = fieldop;
                        store.src = seed.dst;
                        store.idx_reg.kind = OPND_IMM;
                        store.idx_reg.imm = 0;
                        push__instr(store);
                    }
                }
                tok = strtok(NULL, ",");
            }
            (void)d;
            return 1;
        }
    }

    /* Size specifier: 'iK' (integer, K = 8/16/32/64) or 'fK' (float,
       K = 32/64 only -- Chard has no 8/16-bit float type). Both share
       the same 'letter followed by a bit-count' shape, so the letter
       alone (checked first) picks which family, then the same
       digit-parsing applies to either. resolve_size_alias runs first
       so 'char'/'short'/'int'/'long' are accepted as spellings of
       'i8'/'i16'/'i32'/'i64' -- see its own comment for why there's no
       equivalent alias on the float side. */
    const char *szspec = resolve_size_alias(tokens[1]);
    int is_float_decl = 0;
    if (szspec[0] == 'f' && isdigit((unsigned char)szspec[1])) {
        is_float_decl = 1;
        g_uses_float = 1;
    } else if (szspec[0] != 'i' || !isdigit((unsigned char)szspec[1])) {
        fail("expected size specifier (e.g. i8/char, i32/int, or f64) after 'volatile'/'bss'/'local'");
    }
    int size = atoi(szspec + 1) / 8;
    if (size <= 0) size = 1;
    if (is_float_decl && size != 4 && size != 8) {
        fail("float size specifier must be f32 or f64");
    }

    /* name may come with trailing '=' / ';' / '[N]' glued on depending on
       spacing; tokens[2] should be the identifier, possibly followed by
       '[' N ']' (an array size -- 'local' only, see below) or '=' N ';' */
    char name[MAX_SYMLEN];
    strncpy(name, tokens[2], MAX_SYMLEN - 1);
    name[MAX_SYMLEN - 1] = '\0';

    /* 'local iK name[N];' / 'bss iK name[N];' -- an array declaration.
       Checked ahead of the '='/';' stripping since '[' must be found
       in the name token first. 'local'/'bss' give an array form;
       'volatile' does not (an uninitialized array is what bss/local
       already are; 'data iK name[] = v1,...;' covers initialized-
       global with its own '[]'+value-list syntax instead of '[N]').

       An optional '= v1, v2, ...;' initializer is accepted for 'local'
       (parsed from raw_line since it contains commas, which aren't
       split__statements' boundary char). Each value desugars to a
       real runtime 'lastoreN imm > name[i];' store (local arrays have
       no .data equivalent to pre-fill). Fewer than N values leaves
       the rest uninitialized, same as no initializer at all.

       'bss iK name[N];' takes no initializer -- '=' after ']' is
       rejected outright.

       'volatile iK name[N] = v1, v2, ...;' is the explicit-length
       sibling of 'data iK name[] = v1,...;' -- same SEC_DATA storage,
       but the value count must exactly match N (rejected otherwise,
       not zero-filled/truncated), since it's one linker-level .data
       blob with no later store to fill a gap. */
    char *bracket = strchr(name, '[');
    if (bracket) {
        /* No unreachability guard needed here: 'local'/'bss'/'volatile'
           are the only three keywords that can still reach this point
           with a '[' in their name token -- 'data' (the fourth
           SEC_DATA-setting keyword) always returns from its own
           is_data_kw branch above before ever falling through to here,
           since 'data' has a completely different grammar ('[]' plus a
           bare comma list straight after '=', no '[N]' size, no
           tokens[0]/tokens[1]/tokens[2] layout in common with this
           block at all -- see that branch's own comment). So every sec
           value reaching this line is already exactly one of
           SEC_LOCAL/SEC_BSS/SEC_DATA(volatile), each handled below. */
        char *close_bracket = strchr(bracket, ']');
        if (!close_bracket) fail("malformed array declaration: missing ']' (local/bss/volatile iK name[N];)");
        *bracket = '\0'; /* name now holds just the identifier */
        char numbuf[32];
        size_t numlen = (size_t)(close_bracket - bracket - 1);
        if (numlen == 0 || numlen >= sizeof(numbuf)) fail("malformed array declaration: expected an integer inside '[...]'");
        memcpy(numbuf, bracket + 1, numlen);
        numbuf[numlen] = '\0';
        for (size_t ci = 0; ci < numlen; ci++)
            if (!isdigit((unsigned char)numbuf[ci])) fail("array size must be a positive integer literal (local/bss/volatile iK name[N];)");
        int array_len = atoi(numbuf);
        if (array_len <= 0) fail("array size must be a positive integer (local/bss/volatile iK name[N];)");

        /* Does an '=' appear anywhere after the ']'? If so this is the
           initializer-list form; otherwise it's the plain
           'local/bss iK name[N];' declaration as before ('volatile'
           has no plain form -- see below). */
        int has_eq = 0;
        for (int ti = 2; ti < ntok; ti++) {
            if (strchr(tokens[ti], '=')) { has_eq = 1; break; }
        }

        if (sec == SEC_BSS) {
            if (has_eq) {
                char msg[192];
                snprintf(msg, sizeof(msg), "'bss %s[%d]': 'bss' arrays cannot have an initializer (bss is always uninitialized -- did you mean 'data' or 'local'?)", name, array_len);
                fail(msg);
            }
            if (find__decl(name)) failf("redeclaration of '%s'", name);
            DA_ENSURE(decls, decls_cap, ndecls, decl_t);
            decl_t *d = &decls[ndecls++];
            memset(d, 0, sizeof(*d));
            strncpy(d->name, name, MAX_SYMLEN - 1);
            d->section = SEC_BSS;
            d->size_bytes = size;
            d->array_len = array_len;
            d->is_float = is_float_decl;
            return 1;
        }

        if (sec == SEC_DATA || sec == SEC_RODATA) {
            const char *kwname = sec == SEC_RODATA ? "rodata" : "volatile";
            if (!has_eq) {
                char msg[192];
                snprintf(msg, sizeof(msg), "'%s %s[%d]': '%s' declaration requires an initializer (bss iK name[%d]; is the uninitialized form)", kwname, name, array_len, kwname, array_len);
                fail(msg);
            }
            if (find__decl(name)) failf("redeclaration of '%s'", name);

            const char *eq = strchr(raw_line, '=');
            if (!eq) {
                char msg[192];
                snprintf(msg, sizeof(msg), "malformed '%s' array initializer: expected '%s iK name[N] = v1, v2, ...;'", kwname, kwname);
                fail(msg);
            }
            char listbuf[MAX_LINE];
            strncpy(listbuf, eq + 1, MAX_LINE - 1);
            listbuf[MAX_LINE - 1] = '\0';
            char *semi2 = strchr(listbuf, ';');
            if (semi2) *semi2 = '\0';

            long *vals = malloc(sizeof(long) * (size_t)array_len);
            double *fvals = malloc(sizeof(double) * (size_t)array_len);
            if (!vals || !fvals) { perror("malloc"); exit(1); }
            int nvals = 0;
            char *tok = strtok(listbuf, ",");
            while (tok) {
                char *vt = trim(tok);
                if (*vt != '\0') {
                    if (is_float_decl) {
                        if (!is_float_literal(vt)) {
                            char msg[192];
                            snprintf(msg, sizeof(msg), "'%s %s[%d]': initializer values must be plain float literals (got '%s')", kwname, name, array_len, vt);
                            fail(msg);
                        }
                    } else if (!is__number(vt)) {
                        char msg[192];
                        snprintf(msg, sizeof(msg), "'%s %s[%d]': initializer values must be plain integer literals (got '%s')", kwname, name, array_len, vt);
                        fail(msg);
                    }
                    if (nvals >= array_len) {
                        char msg[192];
                        snprintf(msg, sizeof(msg), "'%s %s[%d]': too many initializer values (got more than %d)", kwname, name, array_len, array_len);
                        fail(msg);
                    }
                    if (is_float_decl) fvals[nvals] = atof(vt);
                    else vals[nvals] = parse__number(vt);
                    nvals++;
                }
                tok = strtok(NULL, ",");
            }
            if (nvals != array_len) {
                char msg[192];
                snprintf(msg, sizeof(msg), "'%s %s[%d]': expected exactly %d initializer values, got %d", kwname, name, array_len, array_len, nvals);
                fail(msg);
            }

            DA_ENSURE(decls, decls_cap, ndecls, decl_t);
            decl_t *d = &decls[ndecls++];
            memset(d, 0, sizeof(*d));
            strncpy(d->name, name, MAX_SYMLEN - 1);
            d->section = sec;
            d->size_bytes = size;
            d->is_data_array = 1;
            d->is_float = is_float_decl;
            d->has_init = 1;
            d->array_len = nvals;
            if (is_float_decl) { d->data_fvals = fvals; d->data_fvals_cap = array_len; free(vals); }
            else { d->data_vals = vals; d->data_vals_cap = array_len; free(fvals); }
            return 1;
        }

        decl_t *ad = declare_local_array(name, size, array_len);
        ad->is_float = is_float_decl;

        if (has_eq) {
            const char *eq = strchr(raw_line, '=');
            if (!eq) fail("malformed local array initializer: expected 'local iK name[N] = v1, v2, ...;'");

            char listbuf[MAX_LINE];
            strncpy(listbuf, eq + 1, MAX_LINE - 1);
            listbuf[MAX_LINE - 1] = '\0';
            char *semi = strchr(listbuf, ';');
            if (semi) *semi = '\0';

            long *vals = malloc(sizeof(long) * (size_t)array_len);
            double *fvals = malloc(sizeof(double) * (size_t)array_len);
            if (!vals || !fvals) { perror("malloc"); exit(1); }
            int nvals = 0;
            char *tok = strtok(listbuf, ",");
            while (tok) {
                char *vt = trim(tok);
                if (*vt != '\0') {
                    if (is_float_decl) {
                        if (!is_float_literal(vt)) {
                            char msg[192];
                            snprintf(msg, sizeof(msg), "'local %s[%d]': initializer values must be plain float literals (got '%s')", name, array_len, vt);
                            fail(msg);
                        }
                    } else if (!is__number(vt)) {
                        char msg[192];
                        snprintf(msg, sizeof(msg), "'local %s[%d]': initializer values must be plain integer literals (got '%s')", name, array_len, vt);
                        fail(msg);
                    }
                    if (nvals >= array_len) {
                        char msg[192];
                        snprintf(msg, sizeof(msg), "'local %s[%d]': too many initializer values (got more than %d)", name, array_len, array_len);
                        fail(msg);
                    }
                    if (is_float_decl) fvals[nvals] = atof(vt);
                    else vals[nvals] = parse__number(vt);
                    nvals++;
                }
                tok = strtok(NULL, ",");
            }
            if (nvals == 0) fail("local array initializer list must contain at least one value");

            operand_t arrop;
            parse__operand(name, &arrop);

            if (is_float_decl) {
                /* Mirrors the scalar 'local fK name = val;' desugaring
                   (OP_FMOV seed into the float scratch register,
                   followed by OP_FSTORE) just run once per element
                   instead of once total. There's no float-typed
                   LASTORE (that opcode only ever moves integer
                   registers -- see OP_LALOAD/OP_LASTORE's comment), so
                   this goes through OP_FSTORE directly instead, with
                   dst.local_offset adjusted per element the same way
                   LASTORE's own immediate-index case computes
                   'local_offset - idx*size_bytes' (see declare_local_
                   array's element-layout comment and each backend's
                   OP_LASTORE case) -- every backend's OP_FSTORE already
                   addresses purely from dst.local_offset/local_size/
                   frames_up, with no dependence on the decl still
                   existing in decls[], so no codegen changes are needed
                   for this to work identically on all three targets. */
                for (int vi = 0; vi < nvals; vi++) {
                    instr_t seed; memset(&seed, 0, sizeof(seed));
                    seed.op = OP_FMOV;
                    seed.src.kind = OPND_IMM;
                    seed.src.is_float = 1;
                    seed.src.fimm = fvals[vi];
                    seed.dst.kind = OPND_REG;
                    seed.dst.is_float = 1;
                    seed.dst.reg_num = g_finit_scratch_reg;
                    push__instr(seed);

                    instr_t store; memset(&store, 0, sizeof(store));
                    store.op = OP_FSTORE;
                    store.src = seed.dst;
                    store.dst.kind = OPND_LOCAL;
                    strncpy(store.dst.sym, name, MAX_SYMLEN - 1); /* diagnostics only */
                    store.dst.local_offset = arrop.local_offset - vi * size;
                    store.dst.local_size = size;
                    store.dst.frames_up = arrop.frames_up;
                    push__instr(store);
                }
            } else {
                for (int vi = 0; vi < nvals; vi++) {
                    instr_t seed; memset(&seed, 0, sizeof(seed));
                    seed.op = OP_MOV;
                    seed.src.kind = OPND_IMM;
                    seed.src.imm = vals[vi];
                    seed.dst.kind = OPND_REG;
                    seed.dst.reg_num = g_init_scratch_reg; /* programmer-controlled
                                               via '%iscratchr rN;' -- same
                                               bounce register the scalar
                                               'local iK name = val;' path uses */
                    push__instr(seed);

                    instr_t store; memset(&store, 0, sizeof(store));
                    store.op = OP_LASTORE;
                    store.elem_size = size;
                    store.dst = arrop;
                    store.src = seed.dst;
                    store.idx_reg.kind = OPND_IMM;
                    store.idx_reg.imm = vi;
                    push__instr(store);
                }
            }
            free(vals);
            free(fvals);
        }

        return 1;
    }

    char *semi = strchr(name, ';');
    if (semi) *semi = '\0';
    char *eq = strchr(name, '=');
    if (eq) *eq = '\0';

    /* An initializer ('= N') is optional for 'local' (unlike 'volatile',
       which requires one) and, when present, has to become a real
       runtime store: a stack slot has no linker-level equivalent of a
       .data byte to pre-fill, so 'local i8 x = 5;' desugars to a plain
       declaration immediately followed by 'mv 5 > local's slot'. Parse
       the value here either way, and act on it after the decl exists. */
    long val = 0;
    double fval = 0.0;
    int found_eq = 0;
    for (int i = 2; i < ntok; i++) {
        if (strchr(tokens[i], '=')) {
            char *v = strchr(tokens[i], '=') + 1;
            if (*v == '\0' && i + 1 < ntok) v = tokens[i + 1];
            char vbuf[64];
            strncpy(vbuf, v, sizeof(vbuf) - 1);
            vbuf[sizeof(vbuf)-1] = '\0';
            char *s2 = strchr(vbuf, ';');
            if (s2) *s2 = '\0';
            if (is_float_decl) {
                if (!is_float_literal(vbuf))
                    failf("'local' initializer must be a plain float literal (got '%s'); to initialize from a register, declare then assign separately", vbuf);
                fval = atof(vbuf);
            } else {
                if (!is__number(vbuf))
                    failf("'local' initializer must be a plain numeric literal (got '%s'); to initialize from a register, declare then assign separately", vbuf);
                val = parse__number(vbuf);
            }
            found_eq = 1;
            break;
        }
    }

    if (sec == SEC_DATA && !found_eq) fail("'volatile' declaration requires an initializer");
    if (sec == SEC_RODATA && !found_eq) fail("'rodata' declaration requires an initializer");

    decl_t *d;
    if (is_local) {
        d = declare__local(name, size);
    } else {
        DA_ENSURE(decls, decls_cap, ndecls, decl_t);
        d = &decls[ndecls++];
        memset(d, 0, sizeof(*d));
        strncpy(d->name, name, MAX_SYMLEN - 1);
        d->section = sec;
        d->size_bytes = size;
    }
    d->is_float = is_float_decl;

    if (sec == SEC_DATA || sec == SEC_RODATA) {
        d->has_init = 1;
        if (is_float_decl) d->init_fvalue = fval;
        else d->init_value = val;
    } else if (is_local && found_eq) {
        if (is_float_decl) {
            /* Mirrors the integer path below (mov+store desugaring for
               'local iK name = val;'), but seeds through a float
               register -- f7 by default, programmer-controlled via
               '%rscratchr fN;' (see g_finit_scratch_reg), the
               same role g_init_scratch_reg plays for integer locals.
               f7 (like r12) is an ordinary user-addressable register,
               not a compiler-reserved one, so this bounce is only
               safe when the programmer either isn't using this
               register for anything live, or has moved it elsewhere
               with %rscratchr. */
            instr_t seed; memset(&seed, 0, sizeof(seed));
            seed.op = OP_FMOV;
            seed.src.kind = OPND_IMM;
            seed.src.is_float = 1;
            seed.src.fimm = fval;
            seed.dst.kind = OPND_REG;
            seed.dst.is_float = 1;
            seed.dst.reg_num = g_finit_scratch_reg;
            push__instr(seed);

            instr_t store; memset(&store, 0, sizeof(store));
            store.op = OP_FSTORE;
            store.src = seed.dst;
            store.dst.kind = OPND_LOCAL;
            strncpy(store.dst.sym, name, MAX_SYMLEN - 1); /* diagnostics only */
            store.dst.local_offset = d->local_offset;
            store.dst.local_size = d->size_bytes;
            push__instr(store);
        } else {
            /* Emit 'mv val > SCRATCH-equivalent; store SCRATCH > name;' at
               IR level using the same OP_MOV/OP_STORE pair a hand-written
               'name(*rX) := rX(val);' fused store would produce -- except
               there's no user-chosen register to reuse here, so this
               borrows g_init_scratch_reg (r12 by default, overridable via
               '%iscratchr rN;' -- see the big comment above
               DEFAULT_INIT_SCRATCH_REG) as dedicated initializer scratch.
               This mirrors how the compiler already treats other "value
               has to land in a register before it can reach memory" cases
               (e.g. the fused store's own seed step) rather than
               inventing a new addressing mode just for immediate-to-stack
               stores. */
            instr_t seed; memset(&seed, 0, sizeof(seed));
            seed.op = OP_MOV;
            seed.src.kind = OPND_IMM;
            seed.src.imm = val;
            seed.dst.kind = OPND_REG;
            seed.dst.reg_num = g_init_scratch_reg;
            push__instr(seed);

            instr_t store; memset(&store, 0, sizeof(store));
            store.op = OP_STORE;
            store.src = seed.dst;
            store.dst.kind = OPND_LOCAL;
            strncpy(store.dst.sym, name, MAX_SYMLEN - 1); /* diagnostics only */
            store.dst.local_offset = d->local_offset;
            store.dst.local_size = d->size_bytes;
            push__instr(store);
        }
    }

    return 1;
}

int two_op_has_comma_form(opcode_t op) {
    switch (op) {
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
    case OP_AND: case OP_OR: case OP_XOR:
    case OP_SHL: case OP_SHR:
    case OP_ROTL: case OP_ROTR: case OP_SAT_ADD: case OP_SAT_SUB:
        return 1;
    default:
        return 0;
    }
}

int is_symbolic_op_name(const char *name) {
    static const char *symbolic_names[] = {
        "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "~"
    };
    for (size_t i = 0; i < sizeof(symbolic_names) / sizeof(symbolic_names[0]); i++)
        if (strcmp(name, symbolic_names[i]) == 0) return 1;
    return 0;
}

