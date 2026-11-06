#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AUTH_PASSWORD_SHA256_BYTES 32

/* Standard PBKDF2-HMAC-SHA256 for one 32-byte output block. yield_interval
   may be zero for tests, or non-zero on the single-core C6 to let IDLE run. */
esp_err_t auth_password_pbkdf2_sha256(const char *password,
                                      const uint8_t *salt, size_t salt_length,
                                      uint32_t iterations, uint32_t yield_interval,
                                      uint8_t output[AUTH_PASSWORD_SHA256_BYTES]);
