#pragma once

#include "app_state.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

// Reuses the board sensor bus for BQ27220 and enables the onboard charger.
// Either source may remain unavailable without failing the application.
void app_battery_init(i2c_master_bus_handle_t sensor_bus);

// Updates the UI snapshot without refreshing the display.
void app_battery_read(BatteryStatusState &status);
bool app_battery_read_charging(bool &charging);

// Emits exactly one application-level status line for the supplied snapshot.
void app_battery_log(const BatteryStatusState &status);

// Starts the 60-second battery log timer and the 250 ms charging detector.
// Timer callbacks only post events and never touch the display.
esp_err_t app_battery_start_monitoring();
