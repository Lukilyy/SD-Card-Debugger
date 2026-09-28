#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

struct RtcDateTime {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

// Attaches the PCF8563 device to the shared sensor I2C bus.
esp_err_t sticky_rtc_init(i2c_master_bus_handle_t bus);

// Reads and validates the current RTC date and time without changing it.
esp_err_t sticky_rtc_read(RtcDateTime &date_time);

// Reads a syntactically valid register value for use as an editing hint even
// when VL is set. Callers must never treat this value as trusted time.
esp_err_t sticky_rtc_read_untrusted(RtcDateTime &date_time);

// Copies the validated PCF8563 wall clock into the system clock used by
// ESP-IDF FatFs get_fattime().
esp_err_t sticky_rtc_sync_system_time();

// Writes a validated 2000..2099 date/time. Writing the seconds register with
// bit 7 clear also clears the PCF8563 VL invalid-time flag. The value is read
// back and synchronized to system time before this function succeeds.
esp_err_t sticky_rtc_set(const RtcDateTime &date_time);

// If RTC time is invalid, starts a low-priority monitor input task accepting:
//   rtc set YYYY-MM-DD HH:MM:SS
// A valid RTC is simply synchronized without starting the task.
esp_err_t sticky_rtc_start_console_setup_if_needed();
