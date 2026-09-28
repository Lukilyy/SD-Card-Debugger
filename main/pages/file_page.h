#pragma once

#include <cstddef>
#include <cstdint>

class Canvas;
struct AppState;

void file_page_render(Canvas &canvas, const AppState &state);

// Redraws only the previously selected and newly selected rows in Canvas.
void file_page_render_selection(Canvas &canvas,
                                const AppState &state,
                                size_t previous_selection);

// Returns the visible row index, or -1 when the tap is outside the list.
int file_page_row_at(uint16_t x, uint16_t y);
bool file_page_info_hit_test(uint16_t x, uint16_t y);
bool file_page_back_hit_test(uint16_t x, uint16_t y);
bool file_page_new_hit_test(uint16_t x, uint16_t y);
bool file_page_prev_hit_test(uint16_t x, uint16_t y);
bool file_page_next_hit_test(uint16_t x, uint16_t y);
