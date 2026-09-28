#pragma once

#include "esp_err.h"

// Powers and initializes GT911, then starts the lightweight polling task.
esp_err_t sticky_touch_init();

// Stops the polling task before touch power is removed for deep sleep.
esp_err_t sticky_touch_stop();
