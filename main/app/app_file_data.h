#pragma once

#include "app_state.h"
#include "esp_err.h"

// Files, create-entry, and RTC input state transitions. SD operations finish
// and release SPI2 before the caller redraws the page.
void handle_sd_card_change(AppState &state);
void open_file_browser(AppState &state);
void reload_file_browser(AppState &state);
void select_previous_file_entry(AppState &state);
void select_next_file_entry(AppState &state);
void activate_selected_file_entry(AppState &state);
void open_selected_folder_details(AppState &state);
void file_browser_go_parent(AppState &state);
void file_browser_previous_page(AppState &state);
void file_browser_next_page(AppState &state);

// On success, reload the current directory and SD capacity information.
esp_err_t delete_selected_file(AppState &state);
esp_err_t delete_selected_folder(AppState &state);

void open_new_entry_menu(AppState &state);
void begin_create_entry(AppState &state, CreateEntryType type);
void adjust_rtc_setup_value(AppState &state,
                            DateTimeField field,
                            int direction);
void begin_date_time_number_input(AppState &state, DateTimeField field);
void append_date_time_digit(AppState &state, char digit);
void delete_date_time_digit(AppState &state);
esp_err_t confirm_date_time_number_input(AppState &state);
esp_err_t save_rtc_setup(AppState &state);
void append_create_entry_character(AppState &state, char character);
void backspace_create_entry_name(AppState &state);
void select_create_file_extension(AppState &state, size_t extension_index);
esp_err_t create_entry(AppState &state);
