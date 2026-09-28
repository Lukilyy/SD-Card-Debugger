#include "sticky_rtc.h"

#include <cstdio>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <sys/time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_config.h"

namespace {

constexpr char kTag[] = "sticky_rtc";
constexpr uint32_t kI2cClockHz = 400000;
constexpr int kI2cTimeoutMs = 100;
constexpr uint8_t kControlStatus1Register = 0x00;
constexpr uint8_t kStopFlag = 0x20;
constexpr uint8_t kTimeRegister = 0x02;
constexpr uint8_t kLowVoltageFlag = 0x80;
constexpr size_t kConsoleLineSize = 64;
constexpr uint32_t kConsoleTaskStackSize = 4096;
constexpr UBaseType_t kConsoleTaskPriority = 2;

i2c_master_dev_handle_t s_device = nullptr;
bool s_console_task_running = false;

bool valid_bcd(uint8_t value)
{
    return (value & 0x0FU) <= 9U && ((value >> 4) & 0x0FU) <= 9U;
}

int bcd_to_decimal(uint8_t value)
{
    return static_cast<int>((value >> 4) * 10U + (value & 0x0FU));
}

uint8_t decimal_to_bcd(int value)
{
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

bool valid_date_time(const RtcDateTime &value)
{
    if (value.year < 2000 || value.year > 2099 ||
        value.month < 1 || value.month > 12 ||
        value.day < 1 || value.hour < 0 || value.hour > 23 ||
        value.minute < 0 || value.minute > 59 ||
        value.second < 0 || value.second > 59) {
        return false;
    }

    constexpr int days_per_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    int maximum_day = days_per_month[value.month - 1];
    const bool leap_year =
        (value.year % 4 == 0 && value.year % 100 != 0) || value.year % 400 == 0;
    if (value.month == 2 && leap_year) {
        maximum_day = 29;
    }
    return value.day <= maximum_day;
}

int day_of_week(const RtcDateTime &value)
{
    // Sakamoto algorithm: Sunday=0, matching PCF8563's 0..6 field.
    static constexpr int offsets[] = {
        0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4,
    };
    int year = value.year;
    if (value.month < 3) {
        --year;
    }
    return (year + year / 4 - year / 100 + year / 400 +
            offsets[value.month - 1] + value.day) % 7;
}

bool same_time_with_tolerance(const RtcDateTime &left,
                              const RtcDateTime &right)
{
    struct tm left_tm = {};
    left_tm.tm_year = left.year - 1900;
    left_tm.tm_mon = left.month - 1;
    left_tm.tm_mday = left.day;
    left_tm.tm_hour = left.hour;
    left_tm.tm_min = left.minute;
    left_tm.tm_sec = left.second;
    left_tm.tm_isdst = -1;

    struct tm right_tm = {};
    right_tm.tm_year = right.year - 1900;
    right_tm.tm_mon = right.month - 1;
    right_tm.tm_mday = right.day;
    right_tm.tm_hour = right.hour;
    right_tm.tm_min = right.minute;
    right_tm.tm_sec = right.second;
    right_tm.tm_isdst = -1;

    const time_t left_time = mktime(&left_tm);
    const time_t right_time = mktime(&right_tm);
    if (left_time == static_cast<time_t>(-1) ||
        right_time == static_cast<time_t>(-1)) {
        return false;
    }
    const double difference = std::difftime(right_time, left_time);
    return difference >= 0.0 && difference <= 1.0;
}

bool parse_console_time(const char *line, RtcDateTime &date_time)
{
    if (line == nullptr) {
        return false;
    }
    RtcDateTime parsed = {};
    int consumed = 0;
    const int matched = std::sscanf(
        line,
        "rtc set %d-%d-%d %d:%d:%d %n",
        &parsed.year,
        &parsed.month,
        &parsed.day,
        &parsed.hour,
        &parsed.minute,
        &parsed.second,
        &consumed);
    if (matched != 6 || !valid_date_time(parsed)) {
        return false;
    }
    for (const char *tail = line + consumed; *tail != '\0'; ++tail) {
        if (*tail != '\r' && *tail != '\n' && *tail != ' ' && *tail != '\t') {
            return false;
        }
    }
    date_time = parsed;
    return true;
}

void rtc_console_setup_task(void *)
{
    ESP_LOGW(kTag, "RTC time is invalid. Enter current local time using:");
    ESP_LOGW(kTag, "rtc set YYYY-MM-DD HH:MM:SS");

    char line[kConsoleLineSize] = {};
    size_t length = 0;
    while (true) {
        const int character = std::getchar();
        if (character < 0) {
            std::clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (character == '\r' || character == '\n') {
            if (length == 0) {
                continue;
            }
            line[length] = '\0';
            RtcDateTime requested = {};
            if (!parse_console_time(line, requested)) {
                ESP_LOGW(kTag,
                         "Invalid RTC command. Use: rtc set YYYY-MM-DD HH:MM:SS");
            } else {
                const esp_err_t result = sticky_rtc_set(requested);
                if (result == ESP_OK) {
                    ESP_LOGI(kTag, "RTC setup complete; FAT timestamps are ready");
                    break;
                }
                ESP_LOGE(kTag, "RTC setup failed: %s", esp_err_to_name(result));
            }
            length = 0;
            continue;
        }
        if (length + 1U < sizeof(line)) {
            line[length++] = static_cast<char>(character);
        } else {
            length = 0;
            ESP_LOGW(kTag, "RTC command was too long; input cleared");
        }
    }
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t sticky_rtc_init(i2c_master_bus_handle_t bus)
{
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_device != nullptr) {
        return ESP_OK;
    }

    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = PCF8563_I2C_ADDR;
    config.scl_speed_hz = kI2cClockHz;

    const esp_err_t result = i2c_master_bus_add_device(bus, &config, &s_device);
    if (result == ESP_OK) {
        ESP_LOGI(kTag, "Initialized: I2C address=0x%02X", PCF8563_I2C_ADDR);
    }
    return result;
}

esp_err_t sticky_rtc_read(RtcDateTime &date_time)
{
    if (s_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[7] = {};
    const esp_err_t result = i2c_master_transmit_receive(
        s_device, &kTimeRegister, 1, raw, sizeof(raw), kI2cTimeoutMs);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Read failed: %s", esp_err_to_name(result));
        return result;
    }

    // PCF8563 sets VL when the clock integrity is not guaranteed.
    if ((raw[0] & kLowVoltageFlag) != 0U) {
        ESP_LOGW(kTag, "RTC time invalid: low-voltage flag is set");
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t second = raw[0] & 0x7FU;
    const uint8_t minute = raw[1] & 0x7FU;
    const uint8_t hour = raw[2] & 0x3FU;
    const uint8_t day = raw[3] & 0x3FU;
    const uint8_t month = raw[5] & 0x1FU;
    const uint8_t year = raw[6];
    if (!valid_bcd(second) || !valid_bcd(minute) || !valid_bcd(hour) ||
        !valid_bcd(day) || !valid_bcd(month) || !valid_bcd(year)) {
        ESP_LOGW(kTag, "RTC time invalid: malformed BCD data");
        return ESP_ERR_INVALID_RESPONSE;
    }

    RtcDateTime value = {};
    value.second = bcd_to_decimal(second);
    value.minute = bcd_to_decimal(minute);
    value.hour = bcd_to_decimal(hour);
    value.day = bcd_to_decimal(day);
    value.month = bcd_to_decimal(month);
    const int short_year = bcd_to_decimal(year);
    // PCF8563's century flag toggles at a century boundary; its polarity does
    // not independently identify 19xx versus 20xx. Sticky dates are supported
    // in the modern 2000..2099 range, which is also valid for FAT timestamps.
    value.year = 2000 + short_year;

    if (!valid_date_time(value)) {
        ESP_LOGW(kTag, "RTC time invalid: value is out of range");
        return ESP_ERR_INVALID_RESPONSE;
    }

    date_time = value;
    ESP_LOGI(kTag,
             "Read success: %04d-%02d-%02d %02d:%02d:%02d (century flag=%u)",
             value.year, value.month, value.day,
             value.hour, value.minute, value.second,
             (raw[5] & 0x80U) != 0U ? 1U : 0U);
    return ESP_OK;
}

esp_err_t sticky_rtc_read_untrusted(RtcDateTime &date_time)
{
    if (s_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t raw[7] = {};
    const esp_err_t result = i2c_master_transmit_receive(
        s_device, &kTimeRegister, 1, raw, sizeof(raw), kI2cTimeoutMs);
    if (result != ESP_OK) {
        return result;
    }

    const uint8_t second = raw[0] & 0x7FU;
    const uint8_t minute = raw[1] & 0x7FU;
    const uint8_t hour = raw[2] & 0x3FU;
    const uint8_t day = raw[3] & 0x3FU;
    const uint8_t month = raw[5] & 0x1FU;
    const uint8_t year = raw[6];
    if (!valid_bcd(second) || !valid_bcd(minute) || !valid_bcd(hour) ||
        !valid_bcd(day) || !valid_bcd(month) || !valid_bcd(year)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    RtcDateTime candidate = {};
    candidate.year = 2000 + bcd_to_decimal(year);
    candidate.month = bcd_to_decimal(month);
    candidate.day = bcd_to_decimal(day);
    candidate.hour = bcd_to_decimal(hour);
    candidate.minute = bcd_to_decimal(minute);
    candidate.second = bcd_to_decimal(second);
    if (!valid_date_time(candidate)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    date_time = candidate;
    ESP_LOGW(kTag,
             "Using untrusted RTC registers only as setup defaults: "
             "%04d-%02d-%02d %02d:%02d:%02d (VL=%u)",
             candidate.year, candidate.month, candidate.day,
             candidate.hour, candidate.minute, candidate.second,
             (raw[0] & kLowVoltageFlag) != 0U ? 1U : 0U);
    return ESP_OK;
}

esp_err_t sticky_rtc_sync_system_time()
{
    RtcDateTime date_time = {};
    const esp_err_t read_result = sticky_rtc_read(date_time);
    if (read_result != ESP_OK) {
        return read_result;
    }
    if (date_time.year < 1980 || date_time.year > 2099) {
        ESP_LOGW(kTag, "RTC year cannot be represented by FAT: %d",
                 date_time.year);
        return ESP_ERR_INVALID_RESPONSE;
    }

    struct tm local_time = {};
    local_time.tm_year = date_time.year - 1900;
    local_time.tm_mon = date_time.month - 1;
    local_time.tm_mday = date_time.day;
    local_time.tm_hour = date_time.hour;
    local_time.tm_min = date_time.minute;
    local_time.tm_sec = date_time.second;
    local_time.tm_isdst = -1;

    const time_t seconds = mktime(&local_time);
    if (seconds == static_cast<time_t>(-1)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const timeval system_time = {.tv_sec = seconds, .tv_usec = 0};
    if (settimeofday(&system_time, nullptr) != 0) {
        return ESP_FAIL;
    }

    const time_t readback = time(nullptr);
    struct tm verified_time = {};
    if (localtime_r(&readback, &verified_time) == nullptr ||
        verified_time.tm_year + 1900 != date_time.year ||
        verified_time.tm_mon + 1 != date_time.month ||
        verified_time.tm_mday != date_time.day ||
        verified_time.tm_hour != date_time.hour ||
        verified_time.tm_min != date_time.minute) {
        ESP_LOGE(kTag, "System clock verification failed after RTC sync");
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(kTag,
             "System clock verified for FatFs: %04d-%02d-%02d %02d:%02d:%02d",
             verified_time.tm_year + 1900,
             verified_time.tm_mon + 1,
             verified_time.tm_mday,
             verified_time.tm_hour,
             verified_time.tm_min,
             verified_time.tm_sec);
    return ESP_OK;
}

esp_err_t sticky_rtc_set(const RtcDateTime &date_time)
{
    if (s_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!valid_date_time(date_time)) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t control_status_1 = 0;
    esp_err_t result = i2c_master_transmit_receive(
        s_device,
        &kControlStatus1Register,
        1,
        &control_status_1,
        1,
        kI2cTimeoutMs);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "RTC control-status read failed: %s",
                 esp_err_to_name(result));
        return result;
    }
    if ((control_status_1 & kStopFlag) != 0U) {
        const uint8_t start_payload[] = {
            kControlStatus1Register,
            static_cast<uint8_t>(control_status_1 & ~kStopFlag),
        };
        result = i2c_master_transmit(
            s_device, start_payload, sizeof(start_payload), kI2cTimeoutMs);
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "RTC oscillator start failed: %s",
                     esp_err_to_name(result));
            return result;
        }
        ESP_LOGI(kTag, "RTC STOP flag cleared; oscillator enabled");
    }

    // Register 0x02 is seconds. Its bit 7 is VL and must be written as zero
    // together with a known-good time; clearing it without setting time would
    // incorrectly mark stale register contents as trustworthy.
    const uint8_t payload[] = {
        kTimeRegister,
        decimal_to_bcd(date_time.second),
        decimal_to_bcd(date_time.minute),
        decimal_to_bcd(date_time.hour),
        decimal_to_bcd(date_time.day),
        decimal_to_bcd(day_of_week(date_time)),
        decimal_to_bcd(date_time.month),
        decimal_to_bcd(date_time.year % 100),
    };
    const esp_err_t write_result = i2c_master_transmit(
        s_device, payload, sizeof(payload), kI2cTimeoutMs);
    if (write_result != ESP_OK) {
        ESP_LOGE(kTag, "RTC write failed: %s",
                 esp_err_to_name(write_result));
        return write_result;
    }

    RtcDateTime readback = {};
    const esp_err_t read_result = sticky_rtc_read(readback);
    if (read_result != ESP_OK) {
        ESP_LOGE(kTag, "RTC verification read failed: %s",
                 esp_err_to_name(read_result));
        return read_result;
    }
    if (!same_time_with_tolerance(date_time, readback)) {
        ESP_LOGE(kTag,
                 "RTC verification mismatch: requested=%04d-%02d-%02d %02d:%02d:%02d "
                 "read=%04d-%02d-%02d %02d:%02d:%02d",
                 date_time.year, date_time.month, date_time.day,
                 date_time.hour, date_time.minute, date_time.second,
                 readback.year, readback.month, readback.day,
                 readback.hour, readback.minute, readback.second);
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(kTag,
             "RTC write verified: %04d-%02d-%02d %02d:%02d:%02d; VL is clear",
             readback.year, readback.month, readback.day,
             readback.hour, readback.minute, readback.second);

    const esp_err_t sync_result = sticky_rtc_sync_system_time();
    if (sync_result != ESP_OK) {
        ESP_LOGE(kTag, "System-time sync after RTC write failed: %s",
                 esp_err_to_name(sync_result));
        return sync_result;
    }
    return ESP_OK;
}

esp_err_t sticky_rtc_start_console_setup_if_needed()
{
    RtcDateTime current = {};
    const esp_err_t read_result = sticky_rtc_read(current);
    if (read_result == ESP_OK) {
        return sticky_rtc_sync_system_time();
    }
    if (read_result != ESP_ERR_INVALID_STATE &&
        read_result != ESP_ERR_INVALID_RESPONSE) {
        return read_result;
    }
    if (s_console_task_running) {
        return ESP_OK;
    }

    s_console_task_running = true;
    const BaseType_t created = xTaskCreate(
        rtc_console_setup_task,
        "rtc_setup",
        kConsoleTaskStackSize,
        nullptr,
        kConsoleTaskPriority,
        nullptr);
    if (created != pdPASS) {
        s_console_task_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
