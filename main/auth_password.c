#include "auth_password.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md.h"

esp_err_t auth_password_pbkdf2_sha256(const char *password,
                                      const uint8_t *salt, size_t salt_length,
                                      uint32_t iterations, uint32_t yield_interval,
                                      uint8_t output[AUTH_PASSWORD_SHA256_BYTES])
{
    if (!password || !salt || !salt_length || salt_length > 64 || !iterations || !output) {
        return ESP_ERR_INVALID_ARG;
    }
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return ESP_FAIL;
    mbedtls_md_context_t context;
    mbedtls_md_init(&context);
    uint8_t input[68];
    uint8_t u[AUTH_PASSWORD_SHA256_BYTES];
    memcpy(input, salt, salt_length);
    input[salt_length] = 0;
    input[salt_length + 1] = 0;
    input[salt_length + 2] = 0;
    input[salt_length + 3] = 1; /* First block index in network byte order. */
    int result = mbedtls_md_setup(&context, info, 1);
    if (result == 0) result = mbedtls_md_hmac_starts(&context,
        (const unsigned char *)password, strlen(password));
    if (result == 0) result = mbedtls_md_hmac_update(&context, input, salt_length + 4);
    if (result == 0) result = mbedtls_md_hmac_finish(&context, u);
    if (result == 0) memcpy(output, u, sizeof(u));
    for (uint32_t iteration = 1; result == 0 && iteration < iterations; ++iteration) {
        result = mbedtls_md_hmac_reset(&context);
        if (result == 0) result = mbedtls_md_hmac_update(&context, u, sizeof(u));
        if (result == 0) result = mbedtls_md_hmac_finish(&context, u);
        if (result == 0) {
            for (size_t byte = 0; byte < sizeof(u); ++byte) output[byte] ^= u[byte];
        }
        if (yield_interval && iteration % yield_interval == 0) vTaskDelay(1);
    }
    memset(u, 0, sizeof(u));
    memset(input, 0, sizeof(input));
    mbedtls_md_free(&context);
    return result == 0 ? ESP_OK : ESP_FAIL;
}
