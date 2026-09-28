#pragma once

#include <cstdint>

#include "app_state.h"

class Canvas;

void rtc_setup_page_render(Canvas &canvas, const AppState &state);

// Returns true for a -/+ control and writes its field and direction.
bool rtc_setup_adjustment_at(uint16_t x,
                             uint16_t y,
                             DateTimeField &field,
                             int &direction);
bool rtc_setup_value_field_at(uint16_t x,
                              uint16_t y,
                              DateTimeField &field);
bool rtc_setup_cancel_hit_test(uint16_t x, uint16_t y);
bool rtc_setup_save_hit_test(uint16_t x, uint16_t y);

void rtc_number_input_page_render(Canvas &canvas, const AppState &state);
void rtc_number_input_page_render_value(Canvas &canvas,
                                        const AppState &state);
char rtc_number_input_digit_at(uint16_t x, uint16_t y);
bool rtc_number_input_delete_hit_test(uint16_t x, uint16_t y);
bool rtc_number_input_ok_hit_test(uint16_t x, uint16_t y);
bool rtc_number_input_cancel_hit_test(uint16_t x, uint16_t y);
