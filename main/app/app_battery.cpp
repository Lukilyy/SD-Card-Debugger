#include "app_battery.h"

#include <atomic>

#include "app_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sticky_battery.h"
#include "sticky_charger.h"

namespace {

constexpr char kTag[] = "battery";
constexpr uint64_t kLogPeriodUs = 60ULL * 1000ULL * 1000ULL;
constexpr uint64_t kChargingPollPeriodUs = 250ULL * 1000ULL;

bool s_battery_ready = false;
bool s_charger_ready = false;
esp_timer_handle_t s_log_timer = nullptr;
esp_timer_handle_t s_charging_timer = nullptr;
std::atomic<bool> s_last_charging{false};
std::atomic<bool> s_last_charging_valid{false};

void log_timer_callback(void *)
{
    if (!app_event_post(AppEventType::BatteryLogTick)) {
        ESP_LOGW(kTag, "Battery log event queue is full");
    }
}

void charging_timer_callback(void *)
{
    bool charging = false;
    if (!s_charger_ready || sticky_charger_read(charging) != ESP_OK) {
        return;
    }

    const bool previous_valid =
        s_last_charging_valid.load(std::memory_order_acquire);
    const bool previous =
        s_last_charging.load(std::memory_order_relaxed);
    if (previous_valid && previous == charging) {
        return;
    }
    if (app_event_post(AppEventType::ChargingStateChanged,
                       charging ? 1U : 0U)) {
        s_last_charging.store(charging, std::memory_order_relaxed);
        s_last_charging_valid.store(true, std::memory_order_release);
    }
}

}  // namespace

void app_battery_init(i2c_master_bus_handle_t sensor_bus)
{
    const esp_err_t charger_result = sticky_charger_init();
    s_charger_ready = charger_result == ESP_OK;
    if (!s_charger_ready) {
        ESP_LOGW(kTag, "Charger initialization failed: %s",
                 esp_err_to_name(charger_result));
    }

    const esp_err_t battery_result = sticky_battery_init(sensor_bus);
    s_battery_ready = battery_result == ESP_OK;
    if (!s_battery_ready) {
        ESP_LOGW(kTag, "Battery gauge initialization failed: %s",
                 esp_err_to_name(battery_result));
    }
}

void app_battery_read(BatteryStatusState &status)
{
    status = {};
    status.percent = -1;

    BatteryReading reading = {};
    if (s_battery_ready && sticky_battery_read(reading) == ESP_OK) {
        status.percent = reading.percent;
        status.voltage_mv = reading.voltage_mv;
        status.valid = true;
    }

    bool charging = false;
    if (app_battery_read_charging(charging)) {
        status.charging = charging;
        status.charging_valid = true;
    }
}

bool app_battery_read_charging(bool &charging)
{
    if (!s_charger_ready || sticky_charger_read(charging) != ESP_OK) {
        return false;
    }
    s_last_charging.store(charging, std::memory_order_relaxed);
    s_last_charging_valid.store(true, std::memory_order_release);
    return true;
}

void app_battery_log(const BatteryStatusState &status)
{
    const char *charging = status.charging_valid
                               ? (status.charging ? "YES" : "NO")
                               : "unavailable";
    if (!status.valid) {
        ESP_LOGI(kTag, "Battery: unavailable | Charging: %s", charging);
        return;
    }

    const uint32_t centivolts =
        (static_cast<uint32_t>(status.voltage_mv) + 5U) / 10U;
    ESP_LOGI(kTag, "Battery: %d%% | Voltage: %lu.%02luV | Charging: %s",
             status.percent,
             static_cast<unsigned long>(centivolts / 100U),
             static_cast<unsigned long>(centivolts % 100U),
             charging);
}

esp_err_t app_battery_start_monitoring()
{
    if (s_log_timer != nullptr || s_charging_timer != nullptr) {
        return ESP_OK;
    }

    esp_timer_create_args_t args = {};
    args.callback = log_timer_callback;
    args.name = "battery_log";
    args.skip_unhandled_events = true;
    esp_err_t result = esp_timer_create(&args, &s_log_timer);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_timer_start_periodic(s_log_timer, kLogPeriodUs);
    if (result != ESP_OK) {
        esp_timer_delete(s_log_timer);
        s_log_timer = nullptr;
        return result;
    }

    if (!s_charger_ready) {
        return ESP_OK;
    }
    esp_timer_create_args_t charging_args = {};
    charging_args.callback = charging_timer_callback;
    charging_args.name = "charging_poll";
    charging_args.skip_unhandled_events = true;
    result = esp_timer_create(&charging_args, &s_charging_timer);
    if (result == ESP_OK) {
        result = esp_timer_start_periodic(
            s_charging_timer, kChargingPollPeriodUs);
    }
    if (result != ESP_OK) {
        if (s_charging_timer != nullptr) {
            esp_timer_delete(s_charging_timer);
            s_charging_timer = nullptr;
        }
        ESP_LOGW(kTag, "Charging polling unavailable: %s",
                 esp_err_to_name(result));
        return ESP_OK;
    }
    return ESP_OK;
}
