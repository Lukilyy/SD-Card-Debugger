#include "app_event.h"

#include "freertos/queue.h"

namespace {

// E-Ink refreshes are intentionally synchronous because the display and SD
// card share SPI2. Keep enough input history for a deliberate burst while the
// consumer finishes the current refresh.
constexpr UBaseType_t kQueueLength = 32;
QueueHandle_t s_event_queue = nullptr;

}  // namespace

esp_err_t app_event_init()
{
    if (s_event_queue != nullptr) {
        return ESP_OK;
    }

    s_event_queue = xQueueCreate(kQueueLength, sizeof(AppEvent));
    return s_event_queue != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

bool app_event_post(AppEventType type, uint16_t x, uint16_t y)
{
    const AppEvent event = {type, x, y};
    return s_event_queue != nullptr &&
           xQueueSend(s_event_queue, &event, 0) == pdTRUE;
}

bool app_event_wait(AppEvent &event, TickType_t timeout)
{
    return s_event_queue != nullptr &&
           xQueueReceive(s_event_queue, &event, timeout) == pdTRUE;
}
