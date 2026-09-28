#include "sticky_touch.h"

#include <algorithm>
#include <cstdint>

#include "app_event.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gt911.h"
#include "pin_config.h"
#include "sticky_buzzer.h"
#include "touch_input.h"

namespace {

constexpr char kTag[] = "sticky_touch";
constexpr uint16_t kDisplayWidth = 800;
constexpr uint16_t kDisplayHeight = 480;
constexpr uint16_t kPortraitWidth = 480;
constexpr uint16_t kPortraitHeight = 800;
constexpr int kMinimumSwipeDistance = 120;
constexpr int kMaximumTapMovement = 30;
// Match the responsive polling cadence used by the proven 2048 project.
constexpr TickType_t kPollInterval = pdMS_TO_TICKS(20);
constexpr uint32_t kTaskStackSize = 4096;
constexpr UBaseType_t kTaskPriority = 5;
constexpr TickType_t kStopTimeout = pdMS_TO_TICKS(200);

i2c_master_bus_handle_t s_touch_bus = nullptr;
GT911 s_controller;
TaskHandle_t s_touch_task = nullptr;
volatile bool s_stop_requested = false;
bool s_touching = false;
bool s_press_action_posted = false;
uint16_t s_start_x = 0;
uint16_t s_start_y = 0;
uint16_t s_last_x = 0;
uint16_t s_last_y = 0;

uint16_t scale_coordinate(uint16_t value,
                          uint16_t source_max,
                          uint16_t target_max)
{
    const uint16_t clamped = std::min(value, source_max);
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(clamped) * target_max + source_max / 2U) /
        source_max);
}

void transform_touch_coordinate(uint16_t touch_x,
                                uint16_t touch_y,
                                uint16_t &display_x,
                                uint16_t &display_y)
{
    // Convert the GT911 portrait-oriented coordinates into the display's
    // original landscape framebuffer coordinates.
    const uint16_t portrait_x =
        scale_coordinate(touch_x, kDisplayWidth, kPortraitWidth - 1U);
    const uint16_t mapped_y = std::min(touch_y, kDisplayHeight);
    const uint16_t portrait_y =
        scale_coordinate(kDisplayHeight - mapped_y,
                         kDisplayHeight,
                         kPortraitHeight - 1U);

    const uint16_t framebuffer_x = kDisplayWidth - portrait_y - 1U;
    const uint16_t framebuffer_y = portrait_x;

    // sticky_display rotates the framebuffer by 180 degrees before sending it
    // to the panel. GT911 coordinates are independent of that display-layer
    // rotation, so apply the same rotation here and expose normal physical
    // screen coordinates to the app.
    display_x = kDisplayWidth - framebuffer_x - 1U;
    display_y = kDisplayHeight - framebuffer_y - 1U;
}

bool classify_horizontal_swipe(AppEventType &event)
{
    const int delta_x = static_cast<int>(s_last_x) - s_start_x;
    const int delta_y = static_cast<int>(s_last_y) - s_start_y;
    const int horizontal_distance = delta_x < 0 ? -delta_x : delta_x;
    const int vertical_distance = delta_y < 0 ? -delta_y : delta_y;

    // Require a deliberate horizontal gesture. Taps, short movements, and
    // primarily vertical gestures do not produce page events.
    if (horizontal_distance < kMinimumSwipeDistance ||
        horizontal_distance <= vertical_distance * 2) {
        return false;
    }

    event = delta_x < 0 ? AppEventType::NextPage
                        : AppEventType::PreviousPage;
    return true;
}

bool classify_tap()
{
    const int delta_x = static_cast<int>(s_last_x) - s_start_x;
    const int delta_y = static_cast<int>(s_last_y) - s_start_y;
    const int horizontal_distance = delta_x < 0 ? -delta_x : delta_x;
    const int vertical_distance = delta_y < 0 ? -delta_y : delta_y;
    return horizontal_distance <= kMaximumTapMovement &&
           vertical_distance <= kMaximumTapMovement;
}

bool classify_upward_swipe()
{
    const int delta_x = static_cast<int>(s_last_x) - s_start_x;
    const int delta_y = static_cast<int>(s_last_y) - s_start_y;
    const int horizontal_distance = delta_x < 0 ? -delta_x : delta_x;
    const int vertical_distance = delta_y < 0 ? -delta_y : delta_y;
    return delta_y < 0 && vertical_distance >= kMinimumSwipeDistance &&
           vertical_distance > horizontal_distance * 2;
}

const char *event_name(AppEventType event)
{
    switch (event) {
    case AppEventType::PreviousPage:
        return "PreviousPage";
    case AppEventType::NextPage:
        return "NextPage";
    case AppEventType::RefreshPage:
        return "RefreshPage";
    case AppEventType::ToggleCapacityUnits:
        return "ToggleCapacityUnits";
    case AppEventType::OpenFiles:
        return "OpenFiles";
    case AppEventType::OpenHelp:
        return "OpenHelp";
    case AppEventType::SelectHelpAbout:
        return "SelectHelpAbout";
    case AppEventType::SelectHelpGuide:
        return "SelectHelpGuide";
    case AppEventType::OpenTools:
        return "OpenTools";
    case AppEventType::OpenDiagnostics:
        return "OpenDiagnostics";
    case AppEventType::OpenStorageTest:
        return "OpenStorageTest";
    case AppEventType::StartStorageTest:
        return "StartStorageTest";
    case AppEventType::OpenClearCard:
        return "OpenClearCard";
    case AppEventType::ProceedClearCard:
        return "ProceedClearCard";
    case AppEventType::ConfirmClearCard:
        return "ConfirmClearCard";
    case AppEventType::CancelClearCard:
        return "CancelClearCard";
    case AppEventType::OpenInitializeCard:
        return "OpenInitializeCard";
    case AppEventType::ProceedInitializeCard:
        return "ProceedInitializeCard";
    case AppEventType::ConfirmInitializeCard:
        return "ConfirmInitializeCard";
    case AppEventType::CancelInitializeCard:
        return "CancelInitializeCard";
    case AppEventType::PreviousDiagnosticsPage:
        return "PreviousDiagnosticsPage";
    case AppEventType::NextDiagnosticsPage:
        return "NextDiagnosticsPage";
    case AppEventType::GoBack:
        return "GoBack";
    case AppEventType::PreviousFilePage:
        return "PreviousFilePage";
    case AppEventType::NextFilePage:
        return "NextFilePage";
    case AppEventType::OpenFileRow:
        return "OpenFileRow";
    case AppEventType::OpenFolderDetails:
        return "OpenFolderDetails";
    case AppEventType::RequestFileDelete:
        return "RequestFileDelete";
    case AppEventType::ConfirmFileDelete:
        return "ConfirmFileDelete";
    case AppEventType::CancelFileDelete:
        return "CancelFileDelete";
    case AppEventType::OpenNewEntryMenu:
        return "OpenNewEntryMenu";
    case AppEventType::ChooseNewFolder:
        return "ChooseNewFolder";
    case AppEventType::ChooseNewFile:
        return "ChooseNewFile";
    case AppEventType::AdjustDateTime:
        return "AdjustDateTime";
    case AppEventType::OpenDateTimeField:
        return "OpenDateTimeField";
    case AppEventType::InputDateTimeDigit:
        return "InputDateTimeDigit";
    case AppEventType::DeleteDateTimeDigit:
        return "DeleteDateTimeDigit";
    case AppEventType::ConfirmDateTimeDigit:
        return "ConfirmDateTimeDigit";
    case AppEventType::CancelDateTimeDigit:
        return "CancelDateTimeDigit";
    case AppEventType::SaveDateTime:
        return "SaveDateTime";
    case AppEventType::CancelDateTime:
        return "CancelDateTime";
    case AppEventType::InputNameCharacter:
        return "InputNameCharacter";
    case AppEventType::BackspaceName:
        return "BackspaceName";
    case AppEventType::KeyboardInputRefresh:
        return "KeyboardInputRefresh";
    case AppEventType::ToggleNameCase:
        return "ToggleNameCase";
    case AppEventType::SelectFileExtension:
        return "SelectFileExtension";
    case AppEventType::SubmitCreateEntry:
        return "SubmitCreateEntry";
    case AppEventType::CancelCreateEntry:
        return "CancelCreateEntry";
    case AppEventType::SwipeUp:
        return "SwipeUp";
    case AppEventType::SdCardChanged:
        return "SdCardChanged";
    case AppEventType::BatteryLogTick:
        return "BatteryLogTick";
    case AppEventType::ChargingStateChanged:
        return "ChargingStateChanged";
    case AppEventType::EnterDeepSleep:
        return "EnterDeepSleep";
    }
    return "Unknown";
}

void touch_task(void *)
{
    TickType_t next_poll = xTaskGetTickCount();
    while (true) {
        if (s_stop_requested) {
            break;
        }

        GTPoint point = {};
        const int8_t count = s_controller.read_points(&point, 1);

        if (count > 0) {
            transform_touch_coordinate(point.x, point.y, s_last_x, s_last_y);
            if (!s_touching) {
                s_start_x = s_last_x;
                s_start_y = s_last_y;
                s_press_action_posted =
                    touch_input_post_press(s_start_x, s_start_y);
                if (s_press_action_posted) {
                    ESP_LOGI(kTag, "Button press: (%u,%u)",
                             s_start_x, s_start_y);
                    const esp_err_t beep_result = sticky_buzzer_beep();
                    if (beep_result != ESP_OK) {
                        ESP_LOGW(kTag, "Touch feedback failed: %s",
                                 esp_err_to_name(beep_result));
                    }
                }
            }
            s_touching = true;
        } else if (count == 0 && s_touching) {
            s_touching = false;
            if (s_press_action_posted) {
                s_press_action_posted = false;
                vTaskDelayUntil(&next_poll, kPollInterval);
                continue;
            }
            AppEventType event = AppEventType::NextPage;
            if (classify_horizontal_swipe(event)) {
                ESP_LOGI(kTag,
                         "Swipe: (%u,%u) -> (%u,%u), event=%s",
                         s_start_x, s_start_y, s_last_x, s_last_y,
                         event_name(event));
                if (!app_event_post(event)) {
                    ESP_LOGW(kTag, "App event queue is full");
                }
            } else if (classify_upward_swipe()) {
                ESP_LOGI(kTag,
                         "Up swipe: (%u,%u) -> (%u,%u)",
                         s_start_x, s_start_y, s_last_x, s_last_y);
                if (!app_event_post(AppEventType::SwipeUp)) {
                    ESP_LOGW(kTag, "App event queue is full");
                }
            } else if (classify_tap()) {
                if (touch_input_post_tap(s_last_x, s_last_y)) {
                    ESP_LOGI(kTag, "Action tap: (%u,%u)",
                             s_last_x, s_last_y);
                    const esp_err_t beep_result = sticky_buzzer_beep();
                    if (beep_result != ESP_OK) {
                        ESP_LOGW(kTag, "Touch feedback failed: %s",
                                 esp_err_to_name(beep_result));
                    }
                }
            }
        } else if (count < 0) {
            ESP_LOGW(kTag, "GT911 read failed");
            s_touching = false;
            s_press_action_posted = false;
        }

        vTaskDelayUntil(&next_poll, kPollInterval);
    }

    s_touch_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t sticky_touch_init()
{
    s_stop_requested = false;
    gpio_config_t power_config = {};
    power_config.pin_bit_mask = 1ULL << PIN_TOUCH_EN;
    power_config.mode = GPIO_MODE_OUTPUT;
    power_config.pull_up_en = GPIO_PULLUP_DISABLE;
    power_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    power_config.intr_type = GPIO_INTR_DISABLE;

    esp_err_t result = gpio_config(&power_config);
    if (result != ESP_OK) {
        return result;
    }
    result = gpio_set_level(static_cast<gpio_num_t>(PIN_TOUCH_EN), 1);
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(250));

    i2c_master_bus_config_t bus_config = {};
    bus_config.i2c_port = I2C_NUM_0;
    bus_config.sda_io_num = static_cast<gpio_num_t>(PIN_TOUCH_SDA);
    bus_config.scl_io_num = static_cast<gpio_num_t>(PIN_TOUCH_SCL);
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = 1;
    result = i2c_new_master_bus(&bus_config, &s_touch_bus);
    if (result != ESP_OK) {
        return result;
    }

    if (!s_controller.begin(PIN_TOUCH_INT,
                            PIN_TOUCH_RST,
                            kDisplayWidth,
                            kDisplayHeight,
                            s_touch_bus)) {
        ESP_LOGE(kTag, "GT911 initialization failed");
        return ESP_FAIL;
    }

    uint16_t sensor_width = 0;
    uint16_t sensor_height = 0;
    s_controller.readResolution(sensor_width, sensor_height);
    ESP_LOGI(kTag, "GT911 ready: sensor=%ux%u display=%ux%u address=0x%02X",
             sensor_width, sensor_height,
             kDisplayWidth, kDisplayHeight, s_controller.address());

    if (xTaskCreate(touch_task,
                    "sticky_touch",
                    kTaskStackSize,
                    nullptr,
                    kTaskPriority,
                    &s_touch_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t sticky_touch_stop()
{
    if (s_touch_task == nullptr) {
        return ESP_OK;
    }

    s_stop_requested = true;
    const TickType_t deadline = xTaskGetTickCount() + kStopTimeout;
    while (s_touch_task != nullptr) {
        if (static_cast<int32_t>(xTaskGetTickCount() - deadline) >= 0) {
            ESP_LOGE(kTag, "Timed out while stopping touch task");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(kTag, "Touch polling stopped");
    return ESP_OK;
}
