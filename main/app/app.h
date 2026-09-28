#pragma once

#include "app_event.h"
#include "app_state.h"

class Canvas;

// Draws the selected page into the supplied canvas. It does not refresh hardware.
void render_current_page(Canvas &canvas, const AppState &state);

// Refreshes the current page with a full 1-bit black-and-white waveform.
esp_err_t refresh_current_page(const AppState &state);

// Handles buttons and touch for SD Info and the basic file browser.
esp_err_t handle_app_event(Canvas &canvas,
                           AppState &state,
                           const AppEvent &event);
