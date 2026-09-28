#pragma once

#include "app_state.h"
#include "esp_err.h"

// Reads SD metadata into application state. Errors stay in the state so the
// page can present a stable diagnostic result.
void update_sd_card_info(AppState &state);
void collect_startup_sd_report(AppState &state);
void run_raw_diagnostics(AppState &state);
void run_storage_test(AppState &state);
void run_clear_card(AppState &state);
void prepare_initialize_card(AppState &state);
void run_initialize_card(AppState &state);
