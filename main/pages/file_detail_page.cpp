#include "file_detail_page.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "app_state.h"
#include "capacity_format.h"
#include "canvas.h"
#include "esp_err.h"
#include "page_controls.h"

namespace {

using page_controls::draw_return_icon;

void draw_file_name(Canvas &canvas, const char *name, int y)
{
    constexpr size_t kLineLength = 48;
    char first_line[kLineLength + 1U] = {};
    char second_line[kLineLength + 1U] = {};
    const size_t length = std::strlen(name);
    const size_t first_length = length < kLineLength ? length : kLineLength;
    std::memcpy(first_line, name, first_length);
    first_line[first_length] = '\0';
    canvas.draw_text(204, y, first_line, 2, GrayLevel::Black);

    if (length > first_length) {
        const size_t remaining = length - first_length;
        const size_t second_length =
            remaining < kLineLength ? remaining : kLineLength;
        std::memcpy(second_line, name + first_length, second_length);
        second_line[second_length] = '\0';
        canvas.draw_text(204, y + 28, second_line, 2, GrayLevel::Black);
    }
}

void draw_value(Canvas &canvas, const char *value, int y)
{
    constexpr size_t kLineLength = 46;
    char first_line[kLineLength + 1U] = {};
    char second_line[kLineLength + 1U] = {};
    const size_t length = std::strlen(value);
    const size_t first_length = length < kLineLength ? length : kLineLength;
    std::memcpy(first_line, value, first_length);
    first_line[first_length] = '\0';
    canvas.draw_text(204, y, first_line, 2, GrayLevel::Black);

    if (length > first_length) {
        const size_t remaining = length - first_length;
        const size_t second_length =
            remaining < kLineLength ? remaining : kLineLength;
        std::memcpy(second_line, value + first_length, second_length);
        second_line[second_length] = '\0';
        canvas.draw_text(204, y + 24, second_line, 2, GrayLevel::Black);
    }
}

void format_modified_time(int64_t timestamp, char *output, size_t output_size)
{
    const time_t value = static_cast<time_t>(timestamp);
    struct tm local_time = {};
    if (timestamp <= 0 || localtime_r(&value, &local_time) == nullptr ||
        local_time.tm_year < 80 || local_time.tm_year > 207) {
        std::snprintf(output, output_size, "--");
        return;
    }
    if (std::strftime(output, output_size, "%Y-%m-%d %H:%M",
                      &local_time) == 0U) {
        std::snprintf(output, output_size, "--");
    }
}

void draw_action_button(Canvas &canvas,
                        int x,
                        int width,
                        const char *label,
                        bool filled)
{
    if (filled) {
        canvas.fill_rect(x, 404, width, 52, GrayLevel::Black);
    } else {
        canvas.draw_rect(x, 404, width, 52, GrayLevel::Black);
    }
    const GrayLevel color = filled ? GrayLevel::White : GrayLevel::Black;
    const int text_width = static_cast<int>(std::strlen(label)) * 12;
    canvas.draw_text(x + (width - text_width) / 2, 423,
                     label, 2, color);
}

}  // namespace

bool file_detail_back_hit_test(uint16_t x, uint16_t y)
{
    return page_controls::back_hit_test(x, y);
}

bool file_detail_delete_hit_test(uint16_t x, uint16_t y)
{
    return x >= 540 && x < 784 && y >= 388 && y < 480;
}

bool delete_confirm_cancel_hit_test(uint16_t x, uint16_t y)
{
    return x >= 244 && x < 492 && y >= 388 && y < 480;
}

bool delete_confirm_delete_hit_test(uint16_t x, uint16_t y)
{
    return x >= 516 && x < 784 && y >= 388 && y < 480;
}

void file_detail_page_render(Canvas &canvas, const AppState &state)
{
    canvas.clear(GrayLevel::White);
    canvas.draw_text(24, 28,
                     state.file_action.is_directory
                         ? "FOLDER PROPERTIES"
                         : "FILE PROPERTIES",
                     3, GrayLevel::Black);
    draw_return_icon(canvas);
    canvas.draw_line(24, 76, 776, 76, GrayLevel::Black);

    canvas.draw_text(44, 96, "NAME", 2, GrayLevel::Black);
    draw_file_name(canvas, state.file_action.name, 96);

    canvas.draw_text(44, 164, "TYPE", 2, GrayLevel::Black);
    canvas.draw_text(204, 164,
                     state.file_action.is_directory
                         ? "Folder"
                         : "Regular file",
                     2, GrayLevel::Black);

    if (!state.file_action.is_directory) {
        char size[24] = {};
        format_capacity(state.file_action.size, state.capacity_unit_mode,
                        size, sizeof(size));
        canvas.draw_text(44, 204, "SIZE", 2, GrayLevel::Black);
        canvas.draw_text(204, 204, size, 2, GrayLevel::Black);
    } else {
        char size[24] = "--";
        char contains[64] = "--";
        if (state.file_action.directory_stats_valid) {
            format_capacity(state.file_action.size,
                            state.capacity_unit_mode,
                            size, sizeof(size));
            std::snprintf(
                contains, sizeof(contains), "%llu Files, %llu Folders",
                static_cast<unsigned long long>(
                    state.file_action.contained_file_count),
                static_cast<unsigned long long>(
                    state.file_action.contained_folder_count));
        }
        canvas.draw_text(44, 204, "SIZE", 2, GrayLevel::Black);
        canvas.draw_text(204, 204, size, 2, GrayLevel::Black);
        canvas.draw_text(44, 244, "CONTAINS", 2, GrayLevel::Black);
        canvas.draw_text(204, 244, contains, 2, GrayLevel::Black);
    }

    const int modified_y = state.file_action.is_directory ? 284 : 244;
    char modified[24] = {};
    format_modified_time(state.file_action.modified_time,
                         modified, sizeof(modified));
    canvas.draw_text(44, modified_y, "MODIFIED", 2, GrayLevel::Black);
    canvas.draw_text(204, modified_y, modified, 2, GrayLevel::Black);

    const int path_y = modified_y + 40;
    canvas.draw_text(44, path_y, "PATH", 2, GrayLevel::Black);
    draw_value(canvas, state.file_action.path, path_y);

    if (state.file_action.error != ESP_OK) {
        char error[64] = {};
        std::snprintf(error, sizeof(error), "%s failed: %s",
                      state.file_action.is_directory
                          ? "Folder operation"
                          : "Delete",
                      esp_err_to_name(state.file_action.error));
        canvas.draw_text(44, 382, error, 1, GrayLevel::Black);
    }

    draw_action_button(canvas, 560, 200, "DELETE", false);
}

void delete_confirm_page_render(Canvas &canvas, const AppState &state)
{
    canvas.clear(GrayLevel::White);
    canvas.draw_text(24, 28,
                     state.file_action.is_directory
                         ? "DELETE FOLDER"
                         : "CONFIRM DELETE",
                     3, GrayLevel::Black);
    draw_return_icon(canvas);
    canvas.draw_line(24, 76, 776, 76, GrayLevel::Black);

    canvas.draw_text(44, 116,
                     state.file_action.is_directory
                         ? "Delete this folder?"
                         : "Delete this file?",
                     3, GrayLevel::Black);
    draw_file_name(canvas, state.file_action.name, 172);
    if (state.file_action.is_directory &&
        state.file_action.directory_not_empty) {
        canvas.draw_text(44, 270, "THIS FOLDER IS NOT EMPTY",
                         2, GrayLevel::Black);
        canvas.draw_text(44, 304,
                         "Confirm to delete the folder and all contents.",
                         2, GrayLevel::Black);
    } else {
        canvas.draw_text(44, 270, "This action cannot be undone.",
                         2, GrayLevel::Black);
    }

    draw_action_button(canvas, 272, 200, "CANCEL", false);
    draw_action_button(canvas, 540, 220, "DELETE", true);
}
