#include "rtc_manager.h"

#include <string.h>
#include "app_config.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#define DS3231_REG_TIME    0x00
#define DS3231_REG_STATUS  0x0F
#define DS3231_STATUS_OSF  0x80

static const char *TAG = "rtc";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static rtc_manager_status_t s_status;

static uint8_t from_bcd(uint8_t value) { return (value >> 4) * 10 + (value & 0x0F); }
static uint8_t to_bcd(unsigned value) { return ((value / 10) << 4) | (value % 10); }

static bool leap_year(unsigned year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static unsigned days_in_month(unsigned year, unsigned month)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leap_year(year) ? 29 : days[month - 1];
}

/* Gregorian civil date to days since 1970-01-01, independent of process TZ. */
static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + (int64_t)doe - 719468LL;
}

static void note_result(esp_err_t err)
{
    s_status.last_error = err;
    s_status.online = err == ESP_OK;
}

static esp_err_t read_registers(uint8_t reg, uint8_t *data, size_t length)
{
    if (!s_device) return ESP_ERR_INVALID_STATE;
    esp_err_t err = i2c_master_transmit_receive(s_device, &reg, 1, data, length, 100);
    note_result(err);
    return err;
}

static esp_err_t write_registers(const uint8_t *data, size_t length)
{
    if (!s_device) return ESP_ERR_INVALID_STATE;
    esp_err_t err = i2c_master_transmit(s_device, data, length, 100);
    note_result(err);
    return err;
}

esp_err_t rtc_manager_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = APP_RTC_I2C_SDA_GPIO,
        .scl_io_num = APP_RTC_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
    if (err != ESP_OK) {
        note_result(err);
        return err;
    }
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = APP_RTC_I2C_ADDRESS,
        .scl_speed_hz = APP_RTC_I2C_FREQUENCY_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &device_config, &s_device);
    if (err != ESP_OK) {
        note_result(err);
        return err;
    }
    s_status.initialized = true;
    err = i2c_master_probe(s_bus, APP_RTC_I2C_ADDRESS, 100);
    note_result(err);
    if (err == ESP_OK) ESP_LOGI(TAG, "DS3231 detected at 0x%02x on SDA GPIO%d/SCL GPIO%d",
                               APP_RTC_I2C_ADDRESS, APP_RTC_I2C_SDA_GPIO, APP_RTC_I2C_SCL_GPIO);
    else ESP_LOGW(TAG, "DS3231 not detected: %s; gateway continues without RTC", esp_err_to_name(err));
    return err;
}

esp_err_t rtc_manager_read_utc(time_t *utc, bool *osf)
{
    if (!utc || !osf) return ESP_ERR_INVALID_ARG;
    uint8_t raw[7];
    uint8_t status;
    ESP_RETURN_ON_ERROR(read_registers(DS3231_REG_TIME, raw, sizeof(raw)), TAG, "read time");
    ESP_RETURN_ON_ERROR(read_registers(DS3231_REG_STATUS, &status, 1), TAG, "read status");
    s_status.status_register = status;
    s_status.osf = (status & DS3231_STATUS_OSF) != 0;
    *osf = s_status.osf;

    if (raw[2] & 0x40) return ESP_ERR_INVALID_RESPONSE; /* Require 24-hour register format. */
    unsigned second = from_bcd(raw[0] & 0x7F);
    unsigned minute = from_bcd(raw[1] & 0x7F);
    unsigned hour = from_bcd(raw[2] & 0x3F);
    unsigned day = from_bcd(raw[4] & 0x3F);
    unsigned month = from_bcd(raw[5] & 0x1F);
    unsigned year = 2000 + from_bcd(raw[6]) + ((raw[5] & 0x80) ? 100 : 0);
    if (second > 59 || minute > 59 || hour > 23 || day < 1 || month < 1 || month > 12 ||
        year < 2000 || year > 2199 || day > days_in_month(year, month)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    time_t converted = (time_t)(days_from_civil(year, month, day) * 86400LL +
                                hour * 3600LL + minute * 60LL + second);
    *utc = converted;
    ESP_LOGI(TAG, "RTC read UTC=%lld OSF=%u status=0x%02x", (long long)converted, *osf, status);
    return ESP_OK;
}

esp_err_t rtc_manager_write_utc(time_t utc)
{
    if (!s_device) return ESP_ERR_INVALID_STATE;
    struct tm value;
    gmtime_r(&utc, &value);
    unsigned year = value.tm_year + 1900;
    if (year < 2000 || year > 2199) return ESP_ERR_INVALID_ARG;
    uint8_t data[8] = {
        DS3231_REG_TIME,
        to_bcd(value.tm_sec), to_bcd(value.tm_min), to_bcd(value.tm_hour),
        to_bcd(value.tm_wday ? value.tm_wday : 7), to_bcd(value.tm_mday),
        (uint8_t)(to_bcd(value.tm_mon + 1) | (year >= 2100 ? 0x80 : 0)), to_bcd(year % 100),
    };
    ESP_RETURN_ON_ERROR(write_registers(data, sizeof(data)), TAG, "write UTC");
    uint8_t status;
    ESP_RETURN_ON_ERROR(read_registers(DS3231_REG_STATUS, &status, 1), TAG, "read status");
    uint8_t clear_osf[2] = {DS3231_REG_STATUS, (uint8_t)(status & ~DS3231_STATUS_OSF)};
    ESP_RETURN_ON_ERROR(write_registers(clear_osf, sizeof(clear_osf)), TAG, "clear OSF");
    s_status.status_register = clear_osf[1];
    s_status.osf = false;
    ESP_LOGI(TAG, "RTC updated from trusted system UTC=%lld; OSF cleared", (long long)utc);
    return ESP_OK;
}

void rtc_manager_get_status(rtc_manager_status_t *status)
{
    if (status) *status = s_status;
}
