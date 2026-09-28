#include "sticky_buzzer.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "pin_config.h"

namespace {

// Temporary project-wide mute switch. Change only this value to restore all
// existing button, touch, page-transition, and status feedback tones.
constexpr bool BUZZER_ENABLED = false;
constexpr ledc_mode_t kSpeedMode = LEDC_LOW_SPEED_MODE;
constexpr ledc_timer_t kTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kChannel = LEDC_CHANNEL_0;
constexpr uint32_t kBeepFrequencyHz = 2400;
constexpr uint32_t kBeepDuty = 256;
constexpr TickType_t kBeepDuration = pdMS_TO_TICKS(60);
TimerHandle_t s_beep_timer = nullptr;

esp_err_t hold_buzzer_silent()
{
    const gpio_num_t pin = static_cast<gpio_num_t>(PIN_BUZZER);
    esp_err_t result = gpio_set_level(pin, 0);
    if (result != ESP_OK) {
        return result;
    }

    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << pin;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    result = gpio_config(&config);
    if (result != ESP_OK) {
        return result;
    }
    return gpio_set_level(pin, 0);
}

esp_err_t set_duty(uint32_t duty)
{
    esp_err_t result = ledc_set_duty(kSpeedMode, kChannel, duty);
    if (result != ESP_OK) {
        return result;
    }
    return ledc_update_duty(kSpeedMode, kChannel);
}

void stop_beep(TimerHandle_t)
{
    (void)set_duty(0);
}

}  // namespace

esp_err_t sticky_buzzer_init()
{
    if (!BUZZER_ENABLED) {
        return hold_buzzer_silent();
    }

    ledc_timer_config_t timer_config = {};
    timer_config.speed_mode = kSpeedMode;
    timer_config.timer_num = kTimer;
    timer_config.duty_resolution = LEDC_TIMER_10_BIT;
    timer_config.freq_hz = kBeepFrequencyHz;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(
        ledc_timer_config(&timer_config), "sticky_buzzer", "configure timer");

    ledc_channel_config_t channel_config = {};
    channel_config.gpio_num = PIN_BUZZER;
    channel_config.speed_mode = kSpeedMode;
    channel_config.channel = kChannel;
    channel_config.intr_type = LEDC_INTR_DISABLE;
    channel_config.timer_sel = kTimer;
    channel_config.duty = 0;
    channel_config.hpoint = 0;
    ESP_RETURN_ON_ERROR(
        ledc_channel_config(&channel_config),
        "sticky_buzzer", "configure channel");

    if (s_beep_timer == nullptr) {
        s_beep_timer = xTimerCreate(
            "buzzer_off", kBeepDuration, pdFALSE, nullptr, stop_beep);
        if (s_beep_timer == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

esp_err_t sticky_buzzer_beep()
{
    if (!BUZZER_ENABLED) {
        // Keep GPIO48 at a defined silent level even if callers continue to
        // request feedback through the existing centralized API.
        return gpio_set_level(static_cast<gpio_num_t>(PIN_BUZZER), 0);
    }

    if (s_beep_timer == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(
        ledc_set_freq(kSpeedMode, kTimer, kBeepFrequencyHz),
        "sticky_buzzer", "set frequency");
    ESP_RETURN_ON_ERROR(set_duty(kBeepDuty), "sticky_buzzer", "start beep");
    if (xTimerReset(s_beep_timer, 0) != pdPASS) {
        (void)set_duty(0);
        return ESP_FAIL;
    }
    return ESP_OK;
}
