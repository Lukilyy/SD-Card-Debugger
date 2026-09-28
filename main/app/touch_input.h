#pragma once

#include <cstdint>

struct AppState;

// Publishes the small, read-only UI state required by the touch task.
void touch_input_publish(const AppState &state);

// Converts raw coordinates into page-aware application actions. A true return
// means an actionable command was accepted by the application event queue.
bool touch_input_post_press(uint16_t x, uint16_t y);
bool touch_input_post_tap(uint16_t x, uint16_t y);
