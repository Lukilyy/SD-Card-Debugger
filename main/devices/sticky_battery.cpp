#include "sticky_battery.h"

#include <algorithm>
#include <cstdint>

#include "bq27220.h"
#include "esp_log.h"
#include "pin_config.h"

namespace {

constexpr char kTag[] = "sticky_battery";
constexpr uint32_t kI2cClockHz = 400000;

i2c_master_dev_handle_t s_device = nullptr;

}  // namespace

esp_err_t sticky_battery_init(i2c_master_bus_handle_t bus)
{
    if (bus == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_device != nullptr) {
        return ESP_OK;
    }

    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = BQ27220_I2C_ADDR;
    config.scl_speed_hz = kI2cClockHz;

    esp_err_t result = i2c_master_bus_add_device(bus, &config, &s_device);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "Add I2C device failed: %s", esp_err_to_name(result));
        return result;
    }

    if (!bq27220_probe(s_device)) {
        ESP_LOGE(kTag, "BQ27220 not detected at address 0x%02X",
                 BQ27220_I2C_ADDR);
        i2c_master_bus_rm_device(s_device);
        s_device = nullptr;
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(kTag, "Initialized: I2C address=0x%02X", BQ27220_I2C_ADDR);
    return ESP_OK;
}

esp_err_t sticky_battery_read(BatteryReading &reading)
{
    if (s_device == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t percent = 0;
    uint16_t voltage_mv = 0;
    if (!bq27220_read_state_of_charge(s_device, percent) ||
        !bq27220_read_voltage_mv(s_device, voltage_mv)) {
        return ESP_FAIL;
    }

    reading.percent = std::clamp<int>(percent, 0, 100);
    reading.voltage_mv = voltage_mv;
    return ESP_OK;
}
