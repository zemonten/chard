#include "../../chard.h"

void collect_source_and_macro_defs(FILE *f) {
    char raw[MAX_LINE];
    macro_def_t *cur = NULL;
    int def_line_no = 0;
    enum_collect_t *cur_enum = NULL;
    enum_collect_t cur_enum_storage;
    struct_collect_t *cur_struct = NULL;
    struct_collect_t cur_struct_storage;

    while (fgets(raw, sizeof(raw), f)) {
        g_line_no++;

        /* Explicit line continuation: a '\' as the last non-whitespace
           character of a physical line splices the next physical line
           onto it, repeating for as many consecutive '\'-terminated
           lines as appear, before strip__comment/trim/anything else
           in this loop ever sees the text -- mirrors C's own line-
           splicing phase running strictly before comment stripping and
           tokenization, so a continuation works the same whether it
           falls inside a statement, a macro body line, an enum/struct
           member list, or an 'equ'. This intentionally only strips the
           trailing newline before checking for '\' (not a full trim),
           so trailing whitespace after the '\' -- an invisible typo a
           person is likely to make and never notice -- is NOT treated
           as continuing the line; only a '\' that is truly the final
           character before the newline splices. A '\' that lands
           inside a "..." string literal or after a '//' comment marker
           is intentionally NOT treated as continuation-worthy here
           (see below): splicing happens on raw pre-comment-stripped
           text, so those cases are handled once by locating them
           before deciding whether the '\' counts.
           g_line_no keeps advancing once per physical line consumed
           (so line numbers for anything later in the file stay
           accurate), but the first physical line's number is what
           g_line_no holds when this whole spliced logical line is
           finally handed off below -- matching how a person reading
           the source would point at the line the continuation began,
           not wherever it happened to end. */
        for (;;) {
            size_t len = strlen(raw);
            while (len > 0 && (raw[len-1] == '\n' || raw[len-1] == '\r')) raw[--len] = '\0';

            /* Where would a comment or string start, if any -- found
               against the newline-stripped line so a '\' that is
               really inside '// ...' or a "..." literal is never
               mistaken for a continuation marker. Reuses
               in_string_at()/strstr() the exact same way
               strip__comment() itself will look at this text once
               splicing is done, so 'counts as inside a comment/string'
               agrees between the two decisions. */
            size_t effective_len = len;
            const char *cpos = strstr(raw, "//");
            while (cpos && in_string_at(raw, cpos)) cpos = strstr(cpos + 2, "//");
            if (cpos && (size_t)(cpos - raw) < effective_len) effective_len = (size_t)(cpos - raw);

            if (effective_len > 0 && raw[effective_len - 1] == '\\' && !in_string_at(raw, raw + effective_len - 1)) {
                /* Drop the '\' itself and splice the next physical
                   line directly on -- no inserted space, so a token
                   split mid-identifier across the continuation still
                   reads as one token, matching how the rest of Chard's
                   tokenizer already only cares about whitespace/
                   operator boundaries, never physical line boundaries. */
                raw[effective_len - 1] = '\0';
                size_t cur_len = strlen(raw);
                if (cur_len >= sizeof(raw) - 1) fail("line too long after continuation splicing");
                char cont[MAX_LINE];
                if (!fgets(cont, sizeof(cont), f)) fail("'\\' continuation at end of file: expected another line to follow");
                g_line_no++;
                strncat(raw, cont, sizeof(raw) - cur_len - 1);
                continue;
            }
            break;
        }

        strip__comment(raw);
        char *t = trim(raw);
        if (*t == '\0') continue;

        if (cur) {
            if (strcmp(t, "%endmacro") == 0) { cur = NULL; continue; }
            DA_ENSURE(cur->body, cur->body_cap, cur->nbody, macro_line_t);
            strncpy(cur->body[cur->nbody], t, MAX_LINE - 1);
            cur->body[cur->nbody][MAX_LINE - 1] = '\0';
            cur->nbody++;
            continue;
        }

        /* '| include "file.ch"' / '| data "file.chd"' -- pull in
           another file's lines right here, in place of this one (see
           the big comment above do_include_directive). '|' prefix (not
           '%'), like '| mode'/'| foot': these are whole-program,
           multi-file-structure directives, not compiler-level ones --
           see the '|'-vs-'%' split noted at g_mode's declaration.
           Neither is legal mid-enum/mid-struct (cur_enum/cur_struct,
           checked just below) any more than '%macro' itself would be.
           A mandatory space is required between '|' and 'include'/
           'data' (the glued '|include'/'|data' form is rejected
           outright with a message pointing at the new spelling, same
           as '| mode'/'| foot'). */
        if (strncmp(t, "|include", 8) == 0 && (t[8] == '\0' || isspace((unsigned char)t[8]))) {
            fail("'|include' must have a space after '|' -- write '| include \"file.ch\";'");
        }
        if (t[0] == '|' && isspace((unsigned char)t[1])) {
            const char *word = t + 1;
            while (isspace((unsigned char)*word)) word++;
            if (strncmp(word, "include", 7) == 0 && (word[7] == '\0' || isspace((unsigned char)word[7]))) {
                if (cur_enum) failf("'| include' cannot appear inside 'enum %s { ... }'", cur_enum->name);
                if (cur_struct) failf("'| include' cannot appear inside 'struct %s { ... }'", cur_struct->def->name);
                do_include_directive("| include", 7, word);
                continue;
            }
        }
        if (strncmp(t, "|data", 5) == 0 && (t[5] == '\0' || isspace((unsigned char)t[5]))) {
            fail("'|data' must have a space after '|' -- write '| data \"file.chd\";'");
        }
        if (t[0] == '|' && isspace((unsigned char)t[1])) {
            const char *word = t + 1;
            while (isspace((unsigned char)*word)) word++;
            if (strncmp(word, "data", 4) == 0 && (word[4] == '\0' || isspace((unsigned char)word[4]))) {
                if (cur_enum) failf("'| data' cannot appear inside 'enum %s { ... }'", cur_enum->name);
                if (cur_struct) failf("'| data' cannot appear inside 'struct %s { ... }'", cur_struct->def->name);
                do_include_directive("| data", 4, word);
                continue;
            }
        }
        /* '%entrysym "name"' -- see do_entry_symbol_directive.
           Like | include/| data, illegal mid-enum/mid-struct. */
        if (strncmp(t, "%entrysym", 9) == 0 && (t[9] == '\0' || isspace((unsigned char)t[9]))) {
            if (cur_enum) failf("'%%entrysym' cannot appear inside 'enum %s { ... }'", cur_enum->name);
            if (cur_struct) failf("'%%entrysym' cannot appear inside 'struct %s { ... }'", cur_struct->def->name);
            do_entry_symbol_directive(t);
            continue;
        }
        /* '%sheap N;' -- see do_heap_size_directive. Like the
           other file-level directives above, illegal mid-enum/mid-struct. */
        if (strncmp(t, "%sheap", 6) == 0 && (t[6] == '\0' || isspace((unsigned char)t[6]))) {
            if (cur_enum) failf("'%%sheap' cannot appear inside 'enum %s { ... }'", cur_enum->name);
            if (cur_struct) failf("'%%sheap' cannot appear inside 'struct %s { ... }'", cur_struct->def->name);
            do_heap_size_directive(t);
            continue;
        }
        /* '| mode elf;' / '| mode bare;' -- see do_mode_directive. '|'
           prefix (not '%', unlike every other file-level directive
           here): '|' marks whole-program/runtime-target directives
           (this and 'foot' are the only two), '%' marks compiler-level
           ones -- see the '|'-vs-'%' split noted at g_mode's
           declaration. Checked ahead of the ordinary statement
           pipeline the same way the '%'-prefixed directives are, and
           -- like those -- illegal mid-enum/mid-struct, collected in
           this same pre-pass so it's in effect before any backend-
           emission code runs. */
        if (strncmp(t, "|mode", 5) == 0 && (t[5] == '\0' || isspace((unsigned char)t[5]))) {
            fail("'|mode' must have a space after '|' -- write '| mode elf;' or '| mode bare;'");
        }
        if (t[0] == '|' && isspace((unsigned char)t[1])) {
            const char *word = t + 1;
            while (isspace((unsigned char)*word)) word++;
            if (strncmp(word, "mode", 4) == 0 && (word[4] == '\0' || isspace((unsigned char)word[4]))) {
                if (cur_enum) failf("'| mode' cannot appear inside 'enum %s { ... }'", cur_enum->name);
                if (cur_struct) failf("'| mode' cannot appear inside 'struct %s { ... }'", cur_struct->def->name);
                do_mode_directive(word);
                continue;
            }
        }
        /* '| foot ADDR;' -- see do_foot_directive. Like '| mode', '|'
           prefix (whole-program directive, not compiler-level),
           illegal mid-enum/mid-struct, and collected in the
           pre-pass. A mandatory space is now required between '|' and
           'mode'/'foot' (the old glued '| mode'/'| foot' form is
           rejected outright with a message pointing at the new
           spelling, rather than silently accepted, so a stale file
           gets a clear nudge instead of a confusing parse failure
           somewhere else). */
        if (strncmp(t, "|foot", 5) == 0 && (t[5] == '\0' || isspace((unsigned char)t[5]))) {
            fail("'|foot' must have a space after '|' -- write '| foot ADDR;'");
        }
        if (t[0] == '|' && isspace((unsigned char)t[1])) {
            const char *word = t + 1;
            while (isspace((unsigned char)*word)) word++;
            if (strncmp(word, "foot", 4) == 0 && (word[4] == '\0' || isspace((unsigned char)word[4]))) {
                if (cur_enum) failf("'| foot' cannot appear inside 'enum %s { ... }'", cur_enum->name);
                if (cur_struct) failf("'| foot' cannot appear inside 'struct %s { ... }'", cur_struct->def->name);
                do_foot_directive(word);
                continue;
            }
        }
        /* '%iscratchr rN;' -- see do_init_scratch_directive. Like
           the other file-level directives above, illegal mid-enum/
           mid-struct. Checked with the longer '%rscratchr' prefix
           first below isn't required here since the two keywords
           don't overlap ('%iscratchr' vs '%rscratchr'), but the
           boundary check (t[N] is '\0' or whitespace) still guards
           against a same-prefix directive that doesn't exist yet. */
        if (strncmp(t, "%iscratchr", 10) == 0 && (t[10] == '\0' || isspace((unsigned char)t[10]))) {
            if (cur_enum) failf("'%%iscratchr' cannot appear inside 'enum %s { ... }'", cur_enum->name);
            if (cur_struct) failf("'%%iscratchr' cannot appear inside 'struct %s { ... }'", cur_struct->def->name);
            do_init_scratch_directive(t);
            continue;
        }
        /* '%rscratchr fN;' -- see do_finit_scratch_directive. */
        if (strncmp(t, "%rscratchr", 10) == 0 && (t[10] == '\0' || isspace((unsigned char)t[10]))) {
            if (cur_enum) failf("'%%rscratchr' cannot appear inside 'enum %s { ... }'", cur_enum->name);
            if (cur_struct) failf("'%%rscratchr' cannot appear inside 'struct %s { ... }'", cur_struct->def->name);
            do_finit_scratch_directive(t);
            continue;
        }
        /* '%aliasr NAME = rN;' / '%aliasf NAME = fN;' -- see
           do_alias_directive. Checked with the longer prefix first
           isn't required here (the two keywords diverge at their last
           character, 'r' vs 'f'), but the boundary check (t[8] is
           '\0' or whitespace) still guards the same way every other
           directive here does. */
        if (strncmp(t, "%aliasr", 7) == 0 && (t[7] == '\0' || isspace((unsigned char)t[7]))) {
            if (cur_enum) failf("'%%aliasr' cannot appear inside 'enum %s { ... }'", cur_enum->name);
            if (cur_struct) failf("'%%aliasr' cannot appear inside 'struct %s { ... }'", cur_struct->def->name);
            do_alias_directive(t, 0);
            continue;
        }
        if (strncmp(t, "%aliasf", 7) == 0 && (t[7] == '\0' || isspace((unsigned char)t[7]))) {
            if (cur_enum) failf("'%%aliasf' cannot appear inside 'enum %s { ... }'", cur_enum->name);
            if (cur_struct) failf("'%%aliasf' cannot appear inside 'struct %s { ... }'", cur_struct->def->name);
            do_alias_directive(t, 1);
            continue;
        }

        /* '%argv rN, rM;' -- see do_argv_directive. */
        if (strncmp(t, "%argv", 5) == 0 && (t[5] == '\0' || isspace((unsigned char)t[5]))) {
            if (cur_enum) failf("'%%argv' cannot appear inside 'enum %s { ... }'", cur_enum->name);
            if (cur_struct) failf("'%%argv' cannot appear inside 'struct %s { ... }'", cur_struct->def->name);
            do_argv_directive(t);
            continue;
        }

        /* enum NAME { ... }; -- collected across statement chunks (see
           enum_collect_t's comment). This has to run ahead of
           split__statements/pp__push below, since it consumes raw
           trimmed lines directly the same way the macro-body collector
           above does, rather than going through the ordinary
           statement pipeline at all: an enum member list has no
           opcode/operand shape for that pipeline to make sense of. */
        if (cur_enum) {
            char body[MAX_LINE];
            strncpy(body, t, sizeof(body) - 1);
            body[sizeof(body) - 1] = '\0';
            int closes_here = 0;
            char *close_brace = strchr(body, '}');
            if (close_brace) {
                *close_brace = '\0';
                closes_here = 1;
            }
            char *semi = strchr(body, ';');
            if (semi) *semi = '\0';

            char *piece = strtok(body, ",");
            while (piece) {
                char *pt = trim(piece);
                if (*pt != '\0') enum_finish_member(cur_enum, pt);
                piece = strtok(NULL, ",");
            }
            if (closes_here) cur_enum = NULL;
            continue;
        }

        if (strncmp(t, "enum", 4) == 0 && (t[4] == '\0' || isspace((unsigned char)t[4]))) {
            char *rest = t + 4;
            while (isspace((unsigned char)*rest)) rest++;
            char *brace = strchr(rest, '{');
            char name[MAX_SYMLEN];
            size_t namelen = brace ? (size_t)(brace - rest) : strlen(rest);
            while (namelen > 0 && isspace((unsigned char)rest[namelen - 1])) namelen--;
            if (namelen == 0 || namelen >= MAX_SYMLEN) fail("malformed 'enum': expected 'enum NAME { ... };'");
            memcpy(name, rest, namelen);
            name[namelen] = '\0';
            if (!isalpha((unsigned char)name[0]) && name[0] != '_')
                failf("malformed 'enum': '%s' is not a valid identifier", name);
            if (enum_name_already_declared(name)) failf("'enum %s' is already defined", name);
            if (find__equ(name) || find__macro(name)) failf("'%s' is already defined as something else", name);

            cur_enum_storage.next_value = 0;
            strncpy(cur_enum_storage.name, name, MAX_SYMLEN - 1);
            cur_enum_storage.name[MAX_SYMLEN - 1] = '\0';
            cur_enum = &cur_enum_storage;

            if (!brace) fail("malformed 'enum': expected '{' to open the member list");
            char *after_brace = brace + 1;
            char *close_brace = strchr(after_brace, '}');
            if (close_brace) {
                /* whole 'enum Name { A, B, C };' on one physical line
                   (split__statements hasn't chopped it up in this case
                   because it arrived as one raw fgets line with no
                   embedded ';' before the closing brace) */
                *close_brace = '\0';
                char *piece = strtok(after_brace, ",");
                while (piece) {
                    char *pt = trim(piece);
                    if (*pt != '\0') enum_finish_member(cur_enum, pt);
                    piece = strtok(NULL, ",");
                }
                cur_enum = NULL;
            } else if (*trim(after_brace) != '\0') {
                /* text after '{' but no '}' yet on this line -- collect
                   it as the first batch of members */
                char *piece = strtok(after_brace, ",");
                while (piece) {
                    char *pt = trim(piece);
                    if (*pt != '\0') enum_finish_member(cur_enum, pt);
                    piece = strtok(NULL, ",");
                }
            }
            continue;
        }

        /* struct NAME { ... }; -- collected the same way 'enum' is
           above: field text arrives possibly split across several
           pp_lines-shaped chunks (split__statements treats ';' and '{'/
           '}' as boundaries), so this consumes raw trimmed lines
           directly rather than going through the ordinary statement
           pipeline, matching enum_collect_t's approach exactly. */
        if (cur_struct) {
            char body[MAX_LINE];
            strncpy(body, t, sizeof(body) - 1);
            body[sizeof(body) - 1] = '\0';
            int closes_here = 0;
            char *close_brace = strchr(body, '}');
            if (close_brace) {
                *close_brace = '\0';
                closes_here = 1;
            }
            /* Fields are ';'-terminated, possibly several per line;
               strtok on ';' the same way struct bodies are written
               everywhere else field-declaration-shaped in Chard. */
            char *piece = strtok(body, ";");
            while (piece) {
                char *pt = trim(piece);
                if (*pt != '\0') struct_finish_field(cur_struct->def, pt);
                piece = strtok(NULL, ";");
            }
            if (closes_here) cur_struct = NULL;
            continue;
        }

        if (strncmp(t, "struct", 6) == 0 && (t[6] == '\0' || isspace((unsigned char)t[6]))) {
            char *rest = t + 6;
            while (isspace((unsigned char)*rest)) rest++;
            char *brace = strchr(rest, '{');
            char name[MAX_SYMLEN];
            size_t namelen = brace ? (size_t)(brace - rest) : strlen(rest);
            while (namelen > 0 && isspace((unsigned char)rest[namelen - 1])) namelen--;
            if (namelen == 0 || namelen >= MAX_SYMLEN) fail("malformed 'struct': expected 'struct NAME { ... };'");
            memcpy(name, rest, namelen);
            name[namelen] = '\0';
            if (!isalpha((unsigned char)name[0]) && name[0] != '_')
                failf("malformed 'struct': '%s' is not a valid identifier", name);
            if (find_struct_def(name)) failf("'struct %s' is already defined", name);
            if (find__equ(name) || find__macro(name) || enum_name_already_declared(name))
                failf("'%s' is already defined as something else", name);
            DA_ENSURE(struct_defs, struct_defs_cap, nstruct_defs, struct_def_t);

            struct_def_t *sd = &struct_defs[nstruct_defs++];
            memset(sd, 0, sizeof(*sd));
            strncpy(sd->name, name, MAX_SYMLEN - 1);
            sd->name[MAX_SYMLEN - 1] = '\0';

            cur_struct_storage.def = sd;
            cur_struct = &cur_struct_storage;

            if (!brace) fail("malformed 'struct': expected '{' to open the field list");
            char *after_brace = brace + 1;
            char *close_brace = strchr(after_brace, '}');
            if (close_brace) {
                *close_brace = '\0';
                char *piece = strtok(after_brace, ";");
                while (piece) {
                    char *pt = trim(piece);
                    if (*pt != '\0') struct_finish_field(sd, pt);
                    piece = strtok(NULL, ";");
                }
                cur_struct = NULL;
            } else if (*trim(after_brace) != '\0') {
                char *piece = strtok(after_brace, ";");
                while (piece) {
                    char *pt = trim(piece);
                    if (*pt != '\0') struct_finish_field(sd, pt);
                    piece = strtok(NULL, ";");
                }
            }
            continue;
        }

        if (strncmp(t, "equ", 3) == 0 && (t[3] == '\0' || isspace((unsigned char)t[3]))) {
            char *rest = t + 3;
            while (isspace((unsigned char)*rest)) rest++;
            char *eq = strchr(rest, '=');
            if (!eq) fail("malformed 'equ': expected 'equ NAME = VALUE;'");
            char name[MAX_SYMLEN];
            size_t namelen = (size_t)(eq - rest);
            while (namelen > 0 && isspace((unsigned char)rest[namelen - 1])) namelen--;
            if (namelen == 0 || namelen >= MAX_SYMLEN) fail("malformed 'equ': missing or invalid name");
            memcpy(name, rest, namelen);
            name[namelen] = '\0';
            if (!isalpha((unsigned char)name[0]) && name[0] != '_')
                failf("malformed 'equ': '%s' is not a valid identifier", name);
            if (find__equ(name)) failf("'equ %s' is already defined", name);
            if (find__macro(name)) failf("'%s' is already defined as a macro", name);

            char valbuf[64];
            char *vp = eq + 1;
            while (isspace((unsigned char)*vp)) vp++;
            char *vend = vp;
            while (*vend && *vend != ';' && !isspace((unsigned char)*vend)) vend++;
            size_t vlen = (size_t)(vend - vp);
            if (vlen == 0 || vlen >= sizeof(valbuf)) fail("malformed 'equ': expected an integer value after '='");
            memcpy(valbuf, vp, vlen);
            valbuf[vlen] = '\0';
            if (!is__number(valbuf)) failf("malformed 'equ %s': value must be a plain integer literal", name);

            DA_ENSURE(equs, equs_cap, nequs, equ_def_t);
            equ_def_t *e = &equs[nequs++];
            strncpy(e->name, name, MAX_SYMLEN - 1);
            e->name[MAX_SYMLEN - 1] = '\0';
            e->value = parse__number(valbuf);
            continue;
        }

        if (strncmp(t, "%macro", 6) == 0 && (t[6] == '\0' || isspace((unsigned char)t[6]))) {            DA_ENSURE(macros, macros_cap, nmacros, macro_def_t);
            char *rest = t + 6;
            while (isspace((unsigned char)*rest)) rest++;
            char *paren = strchr(rest, '(');
            if (!paren) fail("malformed '%macro': expected '%macro NAME(params)'");
            char name[MAX_SYMLEN];
            size_t namelen = (size_t)(paren - rest);
            while (namelen > 0 && isspace((unsigned char)rest[namelen - 1])) namelen--;
            if (namelen == 0 || namelen >= MAX_SYMLEN) fail("malformed '%macro': missing or invalid name");
            memcpy(name, rest, namelen);
            name[namelen] = '\0';
            if (find__macro(name)) failf("macro '%s' is already defined", name);

            char *close = strchr(paren, ')');
            if (!close) fail("malformed '%macro': missing ')'");

            macro_def_t *m = &macros[nmacros++];
            memset(m, 0, sizeof(*m));
            strncpy(m->name, name, MAX_SYMLEN - 1);

            char paramlist[MAX_LINE];
            size_t plen = (size_t)(close - paren - 1);
            if (plen >= sizeof(paramlist)) plen = sizeof(paramlist) - 1;
            memcpy(paramlist, paren + 1, plen);
            paramlist[plen] = '\0';

            if (trim(paramlist)[0] != '\0') {
                char *pargs[MAX_MACRO_PARAMS];
                int npargs = split_macro_args(paramlist, pargs);
                for (int i = 0; i < npargs; i++) {
                    if (m->nparams >= MAX_MACRO_PARAMS) failf("macro '%s' has too many parameters", name);
                    strncpy(m->params[m->nparams], pargs[i], MAX_SYMLEN - 1);
                    m->params[m->nparams][MAX_SYMLEN - 1] = '\0';
                    m->nparams++;
                }
            }

            cur = m;
            def_line_no = g_line_no;
            (void)def_line_no;
            continue;
        }

        char **stmt_chunks;
        int nstmts = split__statements(t, &stmt_chunks);
        for (int s = 0; s < nstmts; s++) {
            char *chunk = trim(stmt_chunks[s]);
            if (*chunk != '\0') pp__push(chunk, g_line_no);
        }
    }

    if (cur) failf("macro '%s': missing '%%endmacro'", cur->name);
    if (cur_enum) failf("enum '%s': missing closing '}' (and ';')", cur_enum->name);
    if (cur_struct) failf("struct '%s': missing closing '}' (and ';')", cur_struct->def->name);
}

void parse__file(FILE *f) {
    g_line_no = 0;
    collect_source_and_macro_defs(f);
    substitute_all_equs();
    substitute_all_aliases();
    substitute_all_enums();
    substitute_all_sizeofs();
    expand_all_macro_calls();

    /* Chard_DUMP_EXPANDED=1 prints the fully macro-expanded source to
       stderr before parsing -- useful for debugging a macro the same
       way 'gcc -E' shows preprocessed output. Off by default, zero
       cost when unset. */
    if (getenv("Chard_DUMP_EXPANDED")) {
        for (int li = 0; li < npp_lines; li++)
            fprintf(stderr, "%s:%4d | %s\n", pp_lines[li].filename, pp_lines[li].orig_line_no, pp_lines[li].text);
    }

    /* Storage-table block state: 'volatile { ... }' / 'bss { ... }' /
       'rodata { ... }' / 'data { ... }' groups several decls of the
       same section under one keyword instead of repeating it on every
       line -- see the '<KEYWORD> {' detection below for the full
       rationale. Scoped to this loop (not collected in a pre-pass the
       way enum/struct are) since each member is a completely ordinary
       decl statement once the keyword is prefixed back on -- there's
       no new grammar to learn ahead of parse__decl, just a rewrite of
       't' immediately before the tokenize/parse__decl call already
       below, so it goes through the exact same recovery-checkpoint-
       guarded path (and the exact same parse__decl) as if the
       programmer had spelled out the keyword on every member line
       themselves. NULL when not inside such a block; non-NULL holds
       the keyword text ("volatile"/"bss"/"rodata"/"data") to prefix
       onto every member chunk until the matching '}'. */
    const char *cur_storage_block = NULL;

    for (int li = 0; li < npp_lines; li++) {
        g_filename = pp_lines[li].filename;
        g_line_no = pp_lines[li].orig_line_no;
        char *trimmed_line = pp_lines[li].text;
        g_source_line = trimmed_line; /* pp_lines[] outlives this loop, so
            the pointer stays valid for fail()/failf()'s snippet even
            after this iteration -- see g_source_line's declaration */
        if (*trimmed_line == '\0') continue;

        char **stmt_chunks;
        int nstmts = split__statements(trimmed_line, &stmt_chunks);

        for (int s = 0; s < nstmts; s++) {
            char *t = trim(stmt_chunks[s]);
            if (*t == '\0') continue;

            /* '<KEYWORD> { ... }' -- opens a storage-table block: every
               statement up to the matching '}' is parsed as though it
               had been written '<KEYWORD> <statement>' on its own
               line, so
                   volatile {
                       ascii msg = "hello";
                       i8 num1 = 1;
                       i8 num2 = 2;
                   }
               means exactly what
                   volatile ascii msg = "hello";
                   volatile i8 num1 = 1;
                   volatile i8 num2 = 2;
               already means -- this is purely a way to avoid repeating
               the section keyword on every line, not a new storage
               kind or a new grammar for the members themselves (an
               array/'[N]' member, an 'ascii' member, a plain scalar
               member -- anything parse__decl already accepts after
               that keyword -- works the same inside the block as
               outside it). 'local' is deliberately not included here:
               a block of locals would need its own open_local_frame()
               interaction that plain top-level SEC_LOCAL decls don't
               go through (see declare__local's callers), so mixing it
               into this same block form would either silently do the
               wrong thing or need its own special case -- left out
               entirely rather than half-supported. Checked ahead of
               the ordinary tokenize/parse__decl call below (not
               folded into parse__decl itself) since '{'/'}' aren't
               something any decl grammar has ever needed to know
               about -- this is a block-structuring concern, same
               reasoning as why enum/struct don't teach parse__decl
               about braces either. */
            if (cur_storage_block == NULL &&
                (strncmp(t, "volatile", 8) == 0 || strncmp(t, "bss", 3) == 0 ||
                 strncmp(t, "rodata", 6) == 0 || strncmp(t, "data", 4) == 0)) {
                size_t kwlen = strncmp(t, "volatile", 8) == 0 ? 8 :
                                strncmp(t, "rodata", 6) == 0 ? 6 :
                                strncmp(t, "data", 4) == 0 ? 4 : 3;
                const char *after_kw = t + kwlen;
                if (isspace((unsigned char)*after_kw) || *after_kw == '{') {
                    const char *rest = after_kw;
                    while (isspace((unsigned char)*rest)) rest++;
                    if (*rest == '{' && *trim((char *)rest + 1) == '\0') {
                        cur_storage_block = kwlen == 8 ? "volatile" :
                                             kwlen == 6 ? "rodata" :
                                             kwlen == 4 ? "data" : "bss";
                        continue;
                    }
                }
            }
            if (cur_storage_block != NULL && strcmp(t, "}") == 0) {
                cur_storage_block = NULL;
                continue;
            }
            if (cur_storage_block != NULL) {
                char rewritten[MAX_LINE];
                int wrote = snprintf(rewritten, sizeof(rewritten), "%s %s", cur_storage_block, t);
                if (wrote < 0 || (size_t)wrote >= sizeof(rewritten))
                    failf("statement too long inside '%s { ... }' block", cur_storage_block);
                strncpy(stmt_chunks[s], rewritten, MAX_LINE - 1);
                stmt_chunks[s][MAX_LINE - 1] = '\0';
                t = trim(stmt_chunks[s]);
            }

            /* Recovery checkpoint: a fail()/failf()/fail_fmt() anywhere
               below (however deeply nested -- parse__decl and friends
               call plenty of helpers) longjmps straight back here
               instead of exiting, so one bad statement doesn't stop
               the whole file from being checked. See g_recovery_active's
               comment for why this is scoped to exactly this loop and
               nowhere else.

               Snapshot every counter a single statement's parse can
               grow (decls/instrs/pins/funcs/externs) before attempting
               it, and roll back to the snapshot on longjmp. Most fail()
               call sites in this codebase validate before committing
               (bump the counter only after every check passes), but
               that isn't audited as an invariant across all ~500 call
               sites -- rolling back unconditionally is what actually
               guarantees a half-built entry from a failed statement
               never lingers to confuse a later, valid statement (a
               stale array slot with garbage/zeroed fields that some
               later find__decl-style lookup could otherwise match). */
            int save_ndecls = ndecls, save_nprog = nprog,
                save_nglobal_pins = nglobal_pins,
                save_nfunc_sigs = nfunc_sigs, save_nexterns = nexterns,
                save_local_frame_depth = local_frame_depth,
                save_scope_depth = scope_depth;

            g_recovery_active = 1;
            if (setjmp(g_recovery_point)) {
                g_recovery_active = 0;
                ndecls = save_ndecls;
                nprog = save_nprog;
                nglobal_pins = save_nglobal_pins;
                nfunc_sigs = save_nfunc_sigs;
                nexterns = save_nexterns;
                local_frame_depth = save_local_frame_depth;
                scope_depth = save_scope_depth;
                continue;
            }

            char linebuf[MAX_LINE];
            strncpy(linebuf, t, MAX_LINE - 1);
            linebuf[MAX_LINE - 1] = '\0';

            char *tokens[MAX_TOKENS];
            int ntok = tokenize(linebuf, tokens);
            if (ntok == 0) { g_recovery_active = 0; continue; }

            if (parse__decl(tokens, ntok, t)) { g_recovery_active = 0; continue; }
            if (parse_global_pin(tokens, ntok, t)) { g_recovery_active = 0; continue; }
            if (parse_fused_store(t)) { g_recovery_active = 0; continue; }
            if (parse_instr_line(tokens, ntok, t)) { g_recovery_active = 0; continue; }

            if (strcmp(t, "\\") == 0)
                fail("stray '\\' at end of line: line-continuation requires '\\' to be the very last character before the newline, with no trailing whitespace after it");
            failf("unrecognized line: '%s'", t);
            g_recovery_active = 0;
        }
    }
    g_recovery_active = 0;

    /* If parsing hit any errors, print all of them now and stop before
       codegen ever runs -- codegen assumes a structurally valid
       program (decls that exist, registers in range, etc.), which a
       file that failed to parse cleanly can't guarantee even for the
       statements that themselves parsed fine. */
    if (g_ncollected_errors > 0) {
        for (int i = 0; i < g_ncollected_errors; i++)
            fputs(g_collected_errors[i], stderr);
        if (g_errors_truncated)
            fprintf(stderr, "... additional errors omitted (more than %d)\n", MAX_COLLECTED_ERRORS);
        fprintf(stderr, "%d error%s generated\n", g_ncollected_errors,
                g_ncollected_errors == 1 ? "" : "s");
        exit(1);
    }

    finalize_pending_if_scope();
    if (scope_depth > 0) fail("unclosed 'if', 'while', or 'for': missing '}'");
    if (in_local_frame()) fail("unclosed '@label { ... }' block: missing '}'");
    if (cur_storage_block != NULL) failf("unclosed '%s { ... }' block: missing '}'", cur_storage_block);

    /* Global pins may have been written before the symbol they name was
       declared (Chard has no forward-declaration ordering requirement
       anywhere else either), so existence is checked once here, after

       the whole file -- including all decls -- has been read. */
    for (int i = 0; i < nglobal_pins; i++) {
        decl_t *d = find__decl(global_pins[i].sym);
        if (!d) {
            g_line_no = 0; /* no single line owns this error post-hoc */
            g_source_line = NULL;
            failf("global: unknown symbol '%s'", global_pins[i].sym);
        }
        if (d->section == SEC_LOCAL) {
            g_line_no = 0;
            g_source_line = NULL;
            failf("global: '%s' is a local variable, not a top-level symbol -- pins can only bind data/bss globals", global_pins[i].sym);
        }
    }
}

void apply_entry_symbol_override(void) {
    const char *final_name = NULL;
    if (g_libc_linked) final_name = "main";
    if (g_entry_symbol_override[0] != '\0') final_name = g_entry_symbol_override;
    if (!final_name) return;

    strncpy(entry_label, final_name, MAX_SYMLEN - 1);
    for (int idx = 0; idx < nprog; idx++) {
        if (prog[idx].op == OP_LABEL && prog[idx].is_entry) {
            strncpy(prog[idx].dst.sym, final_name, MAX_SYMLEN - 1);
            break; /* exactly one entry label ever exists */
        }
    }
}

