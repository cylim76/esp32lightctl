#pragma once

#include <stddef.h>

void diagnostics_init(void);
void diagnostics_set_error(const char *source, const char *format, ...);
void diagnostics_get_last_error(char *out, size_t out_len);
const char *diagnostics_reset_reason(void);
