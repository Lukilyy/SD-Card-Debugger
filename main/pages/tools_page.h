#pragma once

#include <cstdint>
#include <cstddef>

class Canvas;
struct AppState;

void tools_page_render(Canvas &canvas);
void diagnostics_page_render(Canvas &canvas, const AppState &state);
void storage_test_page_render(Canvas &canvas, const AppState &state);
void clear_card_page_render(Canvas &canvas, const AppState &state);
void clear_card_confirm_page_render(Canvas &canvas);
void initialize_card_page_render(Canvas &canvas, const AppState &state);
void initialize_card_confirm_page_render(Canvas &canvas,
                                         const AppState &state);

bool tools_page_back_hit_test(uint16_t x, uint16_t y);
bool tools_page_diagnostics_hit_test(uint16_t x, uint16_t y);
bool tools_page_storage_test_hit_test(uint16_t x, uint16_t y);
bool tools_page_clear_card_hit_test(uint16_t x, uint16_t y);
bool tools_page_initialize_card_hit_test(uint16_t x, uint16_t y);
bool diagnostics_page_back_hit_test(uint16_t x, uint16_t y);
bool diagnostics_page_prev_hit_test(uint16_t x, uint16_t y);
bool diagnostics_page_next_hit_test(uint16_t x, uint16_t y);
size_t diagnostics_page_count(const AppState &state);
bool storage_test_page_back_hit_test(uint16_t x, uint16_t y);
bool storage_test_page_start_hit_test(uint16_t x, uint16_t y);
bool clear_card_page_back_hit_test(uint16_t x, uint16_t y);
bool clear_card_page_action_hit_test(uint16_t x, uint16_t y);
bool clear_card_confirm_cancel_hit_test(uint16_t x, uint16_t y);
bool clear_card_confirm_clear_hit_test(uint16_t x, uint16_t y);
bool initialize_card_page_back_hit_test(uint16_t x, uint16_t y);
bool initialize_card_page_action_hit_test(uint16_t x, uint16_t y);
bool initialize_card_confirm_cancel_hit_test(uint16_t x, uint16_t y);
bool initialize_card_confirm_start_hit_test(uint16_t x, uint16_t y);
