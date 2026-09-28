#include "app.h"
#include "app_battery.h"
#include "app_data.h"
#include "board.h"
#include "canvas.h"
#include "esp_log.h"
#include "sticky_buzzer.h"
#include "sticky_buttons.h"
#include "sticky_display.h"
#include "sticky_power.h"
#include "sticky_rtc.h"
#include "sticky_sdcard.h"
#include "sticky_touch.h"

namespace {

constexpr char kTag[] = "sd_card_debugger";
constexpr TickType_t kCardDetectPollInterval = pdMS_TO_TICKS(250);

void show_initial_page(Canvas &canvas, const AppState &state)
{
    render_current_page(canvas, state);
    ESP_ERROR_CHECK(refresh_current_page(state));
}

}  // namespace

extern "C" void app_main()
{
    // The GPIO driver emits verbose pin-configuration INFO lines during the
    // normal board bring-up. Keep its warnings/errors while making the boot
    // report easier to read.
    esp_log_level_set("gpio", ESP_LOG_WARN);
    ESP_LOGI(kTag, "Starting reTerminal Sticky SD Card Debugger");
    sticky_power_log_wakeup_reason();

    ESP_ERROR_CHECK(board_init());
    app_battery_init(board_sensor_i2c_bus());
    ESP_ERROR_CHECK(sticky_display_init());
    const esp_err_t rtc_result = sticky_rtc_init(board_sensor_i2c_bus());
    if (rtc_result != ESP_OK) {
        ESP_LOGW(kTag, "RTC unavailable; file timestamps may be invalid: %s",
                 esp_err_to_name(rtc_result));
    }
    ESP_ERROR_CHECK(sticky_sdcard_init());
    ESP_ERROR_CHECK(app_event_init());
    ESP_ERROR_CHECK(sticky_buttons_init());
    ESP_ERROR_CHECK(sticky_buzzer_init());
    ESP_ERROR_CHECK(sticky_touch_init());
    Canvas *canvas = sticky_display_canvas();
    ESP_ERROR_CHECK(canvas != nullptr ? ESP_OK : ESP_ERR_INVALID_STATE);

    AppState state;
    collect_startup_sd_report(state);
    show_initial_page(*canvas, state);

    if (rtc_result == ESP_OK) {
        const esp_err_t rtc_sync_result = sticky_rtc_sync_system_time();
        if (rtc_sync_result != ESP_OK) {
            ESP_LOGW(kTag,
                     "RTC is not ready; time setup will be requested before creation: %s",
                     esp_err_to_name(rtc_sync_result));
        }
    }

    app_battery_read(state.battery);
    app_battery_log(state.battery);
    const esp_err_t battery_timer_result = app_battery_start_monitoring();
    if (battery_timer_result != ESP_OK) {
        ESP_LOGW(kTag, "Battery log timer unavailable: %s",
                 esp_err_to_name(battery_timer_result));
    }

    bool stable_card_inserted = sticky_sdcard_is_inserted();
    bool candidate_card_inserted = stable_card_inserted;
    uint8_t candidate_samples = 0;

    while (true) {
        AppEvent event;
        if (app_event_wait(event, kCardDetectPollInterval)) {
            const esp_err_t result = handle_app_event(*canvas, state, event);
            if (result != ESP_OK) {
                ESP_LOGE(kTag, "App event failed: %s", esp_err_to_name(result));
            }
            continue;
        }

        const bool card_inserted = sticky_sdcard_is_inserted();
        if (card_inserted != candidate_card_inserted) {
            candidate_card_inserted = card_inserted;
            candidate_samples = 1;
            continue;
        }
        if (candidate_samples < 2) {
            ++candidate_samples;
        }
        if (candidate_samples >= 2 &&
            stable_card_inserted != candidate_card_inserted) {
            stable_card_inserted = candidate_card_inserted;
            ESP_LOGI(kTag, "MicroSD card %s",
                     stable_card_inserted ? "inserted" : "removed");
            const AppEvent card_event = {AppEventType::SdCardChanged, 0, 0};
            const esp_err_t result =
                handle_app_event(*canvas, state, card_event);
            if (result != ESP_OK) {
                ESP_LOGE(kTag, "Card-change event failed: %s",
                         esp_err_to_name(result));
            }
        }
    }
}
