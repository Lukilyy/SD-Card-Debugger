#pragma once

#include <cstdint>

class Canvas;
struct AppState;

void new_entry_menu_render(Canvas &canvas);
void name_input_page_render(Canvas &canvas, const AppState &state);
void name_input_page_render_value(Canvas &canvas, const AppState &state);

bool new_entry_back_hit_test(uint16_t x, uint16_t y);
bool new_folder_hit_test(uint16_t x, uint16_t y);
bool new_file_hit_test(uint16_t x, uint16_t y);

// Returns an ASCII filename character, or zero outside the character grid.
char name_input_character_at(uint16_t x, uint16_t y);
// Returns 0..4 for TXT/JSON/CSV/MD/BIN, or -1 outside the selector.
int name_input_extension_at(uint16_t x, uint16_t y);
bool name_input_case_hit_test(uint16_t x, uint16_t y);
bool name_input_backspace_hit_test(uint16_t x, uint16_t y);
bool name_input_cancel_hit_test(uint16_t x, uint16_t y);
bool name_input_create_hit_test(uint16_t x, uint16_t y);
