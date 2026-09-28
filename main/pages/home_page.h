#pragma once

#include <cstdint>

class Canvas;
struct AppState;

void home_page_render(Canvas &canvas, const AppState &state);

bool home_page_files_hit_test(uint16_t x, uint16_t y);
bool home_page_help_hit_test(uint16_t x, uint16_t y);
bool home_page_tools_hit_test(uint16_t x, uint16_t y);
bool home_page_capacity_units_hit_test(uint16_t x, uint16_t y);
