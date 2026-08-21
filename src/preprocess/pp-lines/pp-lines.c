#include "../../chard.h"

void pp__push(const char *text, int orig_line_no) {
    DA_ENSURE(pp_lines, pp_lines_cap, npp_lines, pp_line_t);
    strncpy(pp_lines[npp_lines].text, text, MAX_LINE - 1);
    pp_lines[npp_lines].text[MAX_LINE - 1] = '\0';
    pp_lines[npp_lines].orig_line_no = orig_line_no;
    pp_lines[npp_lines].filename = g_filename;
    npp_lines++;
}

