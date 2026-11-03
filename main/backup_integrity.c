#include "backup_integrity.h"

#include <string.h>
#include "mbedtls/sha256.h"

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

esp_err_t backup_integrity_sha256(const uint8_t *data, size_t length,
                                  char output[BACKUP_SHA256_HEX_LENGTH + 1])
{
    if ((!data && length) || !output) return ESP_ERR_INVALID_ARG;
    uint8_t digest[32];
    if (mbedtls_sha256(data, length, digest, 0) != 0) return ESP_FAIL;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        output[i * 2] = hex[digest[i] >> 4];
        output[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    output[BACKUP_SHA256_HEX_LENGTH] = '\0';
    return ESP_OK;
}

bool backup_integrity_sha256_matches(const uint8_t *data, size_t length,
                                     const char *expected_hex)
{
    if ((!data && length) || !expected_hex || strlen(expected_hex) != BACKUP_SHA256_HEX_LENGTH) {
        return false;
    }
    char actual[BACKUP_SHA256_HEX_LENGTH + 1];
    if (backup_integrity_sha256(data, length, actual) != ESP_OK) return false;
    uint8_t difference = 0;
    for (size_t i = 0; i < BACKUP_SHA256_HEX_LENGTH; ++i) {
        int expected = hex_value(expected_hex[i]);
        int found = hex_value(actual[i]);
        if (expected < 0) return false;
        difference |= (uint8_t)(expected ^ found);
    }
    return difference == 0;
}
