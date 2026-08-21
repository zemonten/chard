#include "../../chard.h"

void do_entry_symbol_directive(const char *t) {
    if (g_entry_symbol_override[0] != '\0')
        fail("'%entrysym' may only appear once");
    char name[MAX_SYMLEN];
    parse_include_operand("%entrysym", t, 9, name, sizeof(name));
    if (name[0] == '\0') fail("'%entrysym' requires a non-empty name");
    strncpy(g_entry_symbol_override, name, MAX_SYMLEN - 1);
}

void do_mode_directive(const char *t) {
    /* t points at 'mode', the leading '| ' already consumed by the
       caller (mandatory space between '|' and 'mode' is enforced
       there, not here). */
    if (g_mode_seen)
        fail("'| mode' may only appear once");
    const char *rest = t + 4; /* strlen("mode") */
    if (!isspace((unsigned char)*rest))
        fail("malformed '| mode': expected '| mode elf;' or '| mode bare;'");
    while (isspace((unsigned char)*rest)) rest++;

    if (strncmp(rest, "elf", 3) == 0 && !is_ident_char(rest[3])) {
        g_mode = MODE_ELF;
        rest += 3;
    } else if (strncmp(rest, "bare", 4) == 0 && !is_ident_char(rest[4])) {
        g_mode = MODE_BARE;
        rest += 4;
    } else {
        fail("malformed '| mode': expected '| mode elf;' or '| mode bare;'");
    }

    while (isspace((unsigned char)*rest)) rest++;
    if (*rest == ';') rest++;
    while (isspace((unsigned char)*rest)) rest++;
    if (*rest != '\0')
        fail("unexpected text after '| mode elf'/'| mode bare'");

    g_mode_seen = 1;
}

void do_foot_directive(const char *t) {
    /* t points at 'foot', the leading '| ' already consumed by the
       caller (mandatory space between '|' and 'foot' is enforced
       there, not here). */
    if (g_foot_seen)
        fail("'| foot' may only appear once");
    const char *rest = t + 4; /* strlen("foot") */
    while (isspace((unsigned char)*rest)) rest++;
    if (!isdigit((unsigned char)*rest))
        fail("malformed '| foot': expected '| foot ADDR;' (ADDR is a decimal or 0x-prefixed hex address)");

    char *end;
    long val = strtol(rest, &end, 0); /* base 0: accepts '0x...' hex or plain decimal */
    if (val < 0)
        fail("'| foot': address must not be negative");

    const char *after = end;
    while (isspace((unsigned char)*after)) after++;
    if (*after == ';') after++;
    while (isspace((unsigned char)*after)) after++;
    if (*after != '\0')
        fail("unexpected text after '| foot ADDR'");

    g_foot_addr = val;
    g_foot_seen = 1;
}

void do_heap_size_directive(const char *t) {
    if (g_heap_size_seen)
        fail("'%sheap' may only appear once");
    const char *rest = t + 6; /* strlen("%sheap") */
    while (isspace((unsigned char)*rest)) rest++;
    if (!isdigit((unsigned char)*rest))
        fail("malformed '%sheap': expected '%sheap N;' (N is a byte count, optionally suffixed with K or M)");

    char *end;
    long val = strtol(rest, &end, 10);
    if (val <= 0)
        fail("'%sheap': value must be a positive number of bytes");

    long mult = 1;
    if (*end == 'K' || *end == 'k') { mult = 1024L; end++; }
    else if (*end == 'M' || *end == 'm') { mult = 1024L * 1024L; end++; }

    if (val > LONG_MAX / mult)
        fail("'%sheap': value too large");
    val *= mult;

    const char *after = end;
    while (isspace((unsigned char)*after)) after++;
    if (*after == ';') after++;
    while (isspace((unsigned char)*after)) after++;
    if (*after != '\0')
        fail("unexpected text after '%sheap N'");

    g_heap_size_bytes = val;
    g_heap_size_seen = 1;
}

void do_init_scratch_directive(const char *t) {
    if (g_init_scratch_seen)
        fail("'%iscratchr' may only appear once");
    const char *rest = t + 10; /* strlen("%iscratchr") */
    while (isspace((unsigned char)*rest)) rest++;
    if (*rest != 'r' || !isdigit((unsigned char)rest[1]))
        fail("malformed '%iscratchr': expected '%iscratchr rN;' (N is 1-12)");
    rest++;
    char *end;
    long n = strtol(rest, &end, 10);
    if (n < 1 || n > 12)
        fail("'%iscratchr': register must be in r1-r12");

    const char *after = end;
    while (isspace((unsigned char)*after)) after++;
    if (*after == ';') after++;
    while (isspace((unsigned char)*after)) after++;
    if (*after != '\0')
        fail("unexpected text after '%iscratchr rN'");

    g_init_scratch_reg = (int)n;
    g_init_scratch_seen = 1;
}

void do_finit_scratch_directive(const char *t) {
    if (g_finit_scratch_seen)
        fail("'%rscratchr' may only appear once");
    const char *rest = t + 10; /* strlen("%rscratchr") */
    while (isspace((unsigned char)*rest)) rest++;
    if (*rest != 'f' || !isdigit((unsigned char)rest[1]))
        fail("malformed '%rscratchr': expected '%rscratchr fN;' (N is 1-7)");
    rest++;
    char *end;
    long n = strtol(rest, &end, 10);
    if (n < 1 || n > 7)
        fail("'%rscratchr': register must be in f1-f7");

    const char *after = end;
    while (isspace((unsigned char)*after)) after++;
    if (*after == ';') after++;
    while (isspace((unsigned char)*after)) after++;
    if (*after != '\0')
        fail("unexpected text after '%rscratchr fN'");

    g_finit_scratch_reg = (int)n;
    g_finit_scratch_seen = 1;
}

void do_alias_directive(const char *t, int is_float) {
    const char *dirname = is_float ? "%aliasf" : "%aliasr";
    char msg[256];

    const char *rest = t + 8; /* strlen("%aliasr") == strlen("%aliasf") == 8 */
    while (isspace((unsigned char)*rest)) rest++;
    const char *eq = strchr(rest, '=');
    if (!eq) {
        snprintf(msg, sizeof(msg), "malformed '%s': expected '%s NAME = %s;'", dirname, dirname, is_float ? "fN" : "rN (or sp)");
        fail(msg);
    }

    char name[MAX_SYMLEN];
    size_t namelen = (size_t)(eq - rest);
    while (namelen > 0 && isspace((unsigned char)rest[namelen - 1])) namelen--;
    if (namelen == 0 || namelen >= MAX_SYMLEN) {
        snprintf(msg, sizeof(msg), "malformed '%s': missing or invalid name", dirname);
        fail(msg);
    }
    memcpy(name, rest, namelen);
    name[namelen] = '\0';
    if (!isalpha((unsigned char)name[0]) && name[0] != '_') {
        snprintf(msg, sizeof(msg), "malformed '%s': '%s' is not a valid identifier", dirname, name);
        fail(msg);
    }

    if (find__equ(name)) {
        snprintf(msg, sizeof(msg), "'%s %s': '%s' is already defined as an equ constant", dirname, name, name);
        fail(msg);
    }
    if (find__macro(name)) {
        snprintf(msg, sizeof(msg), "'%s %s': '%s' is already defined as a macro", dirname, name, name);
        fail(msg);
    }
    if (find__alias(name)) {
        snprintf(msg, sizeof(msg), "'%s %s' is already defined", dirname, name);
        fail(msg);
    }

    char regtok[16];
    const char *rp = eq + 1;
    while (isspace((unsigned char)*rp)) rp++;
    const char *rend = rp;
    while (*rend && *rend != ';' && !isspace((unsigned char)*rend)) rend++;
    size_t rlen = (size_t)(rend - rp);
    if (rlen == 0 || rlen >= sizeof(regtok)) {
        snprintf(msg, sizeof(msg), "malformed '%s %s': expected a register after '='", dirname, name);
        fail(msg);
    }
    memcpy(regtok, rp, rlen);
    regtok[rlen] = '\0';

    const char *after = rend;
    while (isspace((unsigned char)*after)) after++;
    if (*after == ';') after++;
    while (isspace((unsigned char)*after)) after++;
    if (*after != '\0') {
        snprintf(msg, sizeof(msg), "unexpected text after '%s %s = %s'", dirname, name, regtok);
        fail(msg);
    }

    /* Validate regtok against parse__register itself, rather than
       hand-rolling a second copy of "what counts as a valid register"
       -- guarantees an alias can never expand to something
       parse__register would reject anyway (a typo like 'r13' or 'f9'
       fails right here, at the alias definition site, instead of
       silently substituting bad text that only fails much later at
       every one of its use sites with a confusing error). */
    operand_t probe;
    if (!parse__register(regtok, &probe)) {
        snprintf(msg, sizeof(msg), "'%s %s = %s': '%s' is not a valid register", dirname, name, regtok, regtok);
        fail(msg);
    }
    if (is_float && !probe.is_float) {
        snprintf(msg, sizeof(msg), "'%%aliasf %s = %s': expected a float register (f1-f7), not an integer register", name, regtok);
        fail(msg);
    }
    if (!is_float && probe.is_float) {
        snprintf(msg, sizeof(msg), "'%%aliasr %s = %s': expected an integer register (r1-r12) or sp, not a float register", name, regtok);
        fail(msg);
    }

    DA_ENSURE(aliases, aliases_cap, naliases, alias_def_t);
    alias_def_t *a = &aliases[naliases++];
    strncpy(a->name, name, MAX_SYMLEN - 1);
    a->name[MAX_SYMLEN - 1] = '\0';
    strncpy(a->regtext, regtok, sizeof(a->regtext) - 1);
    a->regtext[sizeof(a->regtext) - 1] = '\0';
}

void do_argv_directive(const char *t) {
    if (g_argv_seen)
        fail("'%argv' may only appear once");
    const char *rest = t + 5; /* strlen("%argv") */
    while (isspace((unsigned char)*rest)) rest++;

    operand_t argc_op = {0};
    if (rest[0] != 'r' || !isdigit((unsigned char)rest[1]))
        fail("malformed '%argv': expected '%argv rN, rM;' (rN = argc, rM = argv)");
    if (!parse__register(rest, &argc_op) || argc_op.is_float)
        fail("'%argv': first register must be a plain integer register r1-r12");
    rest++; /* past 'r' */
    while (isdigit((unsigned char)*rest)) rest++;

    while (isspace((unsigned char)*rest)) rest++;
    if (*rest != ',')
        fail("malformed '%argv': expected '%argv rN, rM;' (missing ',')");
    rest++;
    while (isspace((unsigned char)*rest)) rest++;

    operand_t argv_op = {0};
    if (rest[0] != 'r' || !isdigit((unsigned char)rest[1]))
        fail("malformed '%argv': expected '%argv rN, rM;' (rN = argc, rM = argv)");
    if (!parse__register(rest, &argv_op) || argv_op.is_float)
        fail("'%argv': second register must be a plain integer register r1-r12");
    rest++; /* past 'r' */
    while (isdigit((unsigned char)*rest)) rest++;

    if (argc_op.reg_num == argv_op.reg_num)
        fail("'%argv': argc and argv registers must be different");

    while (isspace((unsigned char)*rest)) rest++;
    if (*rest == ';') rest++;
    while (isspace((unsigned char)*rest)) rest++;
    if (*rest != '\0')
        fail("unexpected text after '%argv rN, rM'");

    g_argv_argc_reg = argc_op.reg_num;
    g_argv_argv_reg = argv_op.reg_num;
    g_argv_seen = 1;
}

