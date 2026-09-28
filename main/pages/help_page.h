#pragma once

#include <cstdint>

class Canvas;
struct AppState;

void help_page_render(Canvas &canvas, const AppState &state);
void help_page_render_battery_status(Canvas &canvas, const AppState &state);
bool help_page_back_hit_test(uint16_t x, uint16_t y);
bool help_page_about_hit_test(uint16_t x, uint16_t y);
bool help_page_guide_hit_test(uint16_t x, uint16_t y);
