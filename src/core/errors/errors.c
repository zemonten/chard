#include "../../chard.h"

void format_error(char *out, size_t out_sz, const char *msg) {
    char loc[MAX_LINE + 64];
    if (g_line_no > 0)
        snprintf(loc, sizeof(loc), "  --> %s:%d\n", g_filename, g_line_no);
    else
        snprintf(loc, sizeof(loc), "  --> %s\n", g_filename);

    if (g_line_no > 0 && g_source_line) {
        snprintf(out, out_sz, "error: %s\n%s   |\n%4d | %s\n   |\n",
                 msg, loc, g_line_no, g_source_line);
    } else {
        snprintf(out, out_sz, "error: %s\n%s", msg, loc);
    }
}

void raise_error(const char *msg) {
    if (g_recovery_active) {
        if (g_ncollected_errors < MAX_COLLECTED_ERRORS) {
            format_error(g_collected_errors[g_ncollected_errors],
                         sizeof(g_collected_errors[0]), msg);
            g_ncollected_errors++;
        } else {
            g_errors_truncated = 1;
        }
        longjmp(g_recovery_point, 1);
    }
    char buf[MAX_LINE + 128];
    format_error(buf, sizeof(buf), msg);
    fputs(buf, stderr);
    exit(1);
}

void fail(const char *msg) {
    raise_error(msg);
}

void failf(const char *fmt, const char *arg) {
    char msg[MAX_LINE];
    snprintf(msg, sizeof(msg), fmt, arg);
    raise_error(msg);
}

void fail_fmt(const char *fmt, ...) {
    char msg[MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    raise_error(msg);
}

