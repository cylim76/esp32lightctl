#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define BACKUP_FORMAT_VERSION 1
#define BACKUP_MAX_FILE_BYTES (48U * 1024U)
#define BACKUP_IMPORT_TOKEN_LENGTH 64

typedef struct {
    size_t schedules;
    size_t ir_codes;
    size_t extension_sections;
    size_t restored_extensions;
    size_t skipped_sections;
    bool ir_section_available;
    bool requires_zigbee_repair;
    char token[BACKUP_IMPORT_TOKEN_LENGTH + 1];
    char message[192];
} backup_import_summary_t;

/* Must run after storage_manager_init() and before settings_init(). */
esp_err_t backup_restore_recover_pending(void);

esp_err_t backup_restore_export_json(char **json_out, size_t *length_out,
                                     char *filename, size_t filename_size);
esp_err_t backup_restore_validate_import(const char *json, size_t length,
                                         backup_import_summary_t *summary);
esp_err_t backup_restore_commit_import(const char *token,
                                       backup_import_summary_t *summary);
void backup_restore_discard_staged(void);
