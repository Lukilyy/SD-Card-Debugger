#pragma once

#include <cstdint>

class Canvas;
struct AppState;

void file_detail_page_render(Canvas &canvas, const AppState &state);
void delete_confirm_page_render(Canvas &canvas, const AppState &state);

bool file_detail_back_hit_test(uint16_t x, uint16_t y);
bool file_detail_delete_hit_test(uint16_t x, uint16_t y);
bool delete_confirm_cancel_hit_test(uint16_t x, uint16_t y);
bool delete_confirm_delete_hit_test(uint16_t x, uint16_t y);
