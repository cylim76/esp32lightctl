#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define BACKUP_SHA256_HEX_LENGTH 64

esp_err_t backup_integrity_sha256(const uint8_t *data, size_t length,
                                  char output[BACKUP_SHA256_HEX_LENGTH + 1]);
bool backup_integrity_sha256_matches(const uint8_t *data, size_t length,
                                     const char *expected_hex);
