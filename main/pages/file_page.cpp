#include "file_page.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app_state.h"
#include "capacity_format.h"
#include "canvas.h"
#include "esp_err.h"
#include "page_controls.h"

namespace {

using page_controls::draw_return_icon;
using page_controls::draw_three_pixel_diagonal;

constexpr int kListX = 24;
constexpr int kListY = 100;
constexpr int kListWidth = 752;
constexpr int kRowHeight = 46;
constexpr int kRowStep = 50;
constexpr int kMaximumRows = kFileBrowserEntriesPerPage;

void draw_list_row(Canvas &canvas,
                   const FileBrowserState &files,
                   size_t row,
                   CapacityUnitMode unit_mode)
{
    if (row >= files.entry_count || row >= kMaximumRows) {
        return;
    }

    const StickySdDirectoryEntry &entry = files.entries[row];
    const int y = kListY + static_cast<int>(row) * kRowStep;
    const bool selected = files.selected == row;
    const GrayLevel background = selected ? GrayLevel::Black : GrayLevel::White;
    const GrayLevel foreground = selected ? GrayLevel::White : GrayLevel::Black;
    canvas.fill_rect(kListX, y, kListWidth, kRowHeight, background);
    canvas.draw_rect(kListX, y, kListWidth, kRowHeight, GrayLevel::Black);

    char name[55] = {};
    std::snprintf(name,
                  sizeof(name),
                  "%s %.38s",
                  entry.is_directory ? "[D]" : "[F]",
                  entry.name);
    canvas.draw_text(kListX + 12, y + 13, name, 2, foreground);

    if (!entry.is_directory) {
        char size[24] = {};
        format_capacity(entry.size, unit_mode, size, sizeof(size));
        canvas.draw_text(574, y + 13, size, 2, foreground);
    }
    canvas.draw_text(718, y + 13, "...", 2, foreground);
}

void draw_button(Canvas &canvas,
                 int center_x,
                 bool points_right)
{
    const int center_y = 439;
    if (points_right) {
        canvas.fill_rect(center_x - 13, center_y - 1,
                         12, 3, GrayLevel::Black);
        draw_three_pixel_diagonal(canvas,
                                  center_x + 12, center_y,
                                  -1, -1, 7);
        draw_three_pixel_diagonal(canvas,
                                  center_x + 12, center_y,
                                  -1, 1, 7);
    } else {
        canvas.fill_rect(center_x + 2, center_y - 1,
                         12, 3, GrayLevel::Black);
        draw_three_pixel_diagonal(canvas,
                                  center_x - 12, center_y,
                                  1, -1, 7);
        draw_three_pixel_diagonal(canvas,
                                  center_x - 12, center_y,
                                  1, 1, 7);
    }
}

}  // namespace

int file_page_row_at(uint16_t x, uint16_t y)
{
    if (x < kListX || x >= kListX + kListWidth || y < kListY) {
        return -1;
    }
    const int row = (y - kListY) / kRowStep;
    const int row_offset = (y - kListY) % kRowStep;
    return row < kMaximumRows && row_offset < kRowHeight ? row : -1;
}

bool file_page_info_hit_test(uint16_t x, uint16_t y)
{
    return x >= 700 && x < 776 && file_page_row_at(x, y) >= 0;
}

bool file_page_back_hit_test(uint16_t x, uint16_t y)
{
    return page_controls::back_hit_test(x, y);
}

bool file_page_new_hit_test(uint16_t x, uint16_t y)
{
    return x >= 584 && x < 704 && y >= 12 && y < 72;
}

bool file_page_prev_hit_test(uint16_t x, uint16_t y)
{
    return page_controls::previous_page_hit_test(x, y);
}

bool file_page_next_hit_test(uint16_t x, uint16_t y)
{
    return page_controls::next_page_hit_test(x, y);
}

void file_page_render_selection(Canvas &canvas,
                                const AppState &state,
                                size_t previous_selection)
{
    draw_list_row(canvas, state.files, previous_selection,
                  state.capacity_unit_mode);
    if (state.files.selected != previous_selection) {
        draw_list_row(canvas, state.files, state.files.selected,
                      state.capacity_unit_mode);
    }
}

void file_page_render(Canvas &canvas, const AppState &state)
{
    const FileBrowserState &files = state.files;
    canvas.clear(GrayLevel::White);
    draw_return_icon(canvas);
    if (files.valid) {
        canvas.draw_rect(608, 18, 82, 42, GrayLevel::Black);
        constexpr int kNewTextWidth = 3 * 6 * 2;
        canvas.draw_text(608 + (82 - kNewTextWidth) / 2, 32,
                         "NEW", 2, GrayLevel::Black);
    }

    char sd_path[kFileBrowserPathSize + 6U] = "FILES ";
    size_t sd_path_length = 6U;
    const size_t directory_path_length = std::strlen(files.path);
    const size_t copy_length = std::min(
        directory_path_length,
        sizeof(sd_path) - sd_path_length - 1U);
    std::memcpy(sd_path + sd_path_length, files.path, copy_length);
    sd_path_length += copy_length;
    sd_path[sd_path_length] = '\0';

    char displayed_path[47] = {};
    const size_t path_length = sd_path_length;
    if (path_length < sizeof(displayed_path)) {
        std::memcpy(displayed_path, sd_path, path_length);
        displayed_path[path_length] = '\0';
    } else {
        constexpr size_t kPrefixLength = 3;
        constexpr size_t kSuffixLength =
            sizeof(displayed_path) - kPrefixLength - 1U;
        std::memcpy(displayed_path, "...", kPrefixLength);
        std::memcpy(displayed_path + kPrefixLength,
                    sd_path + path_length - kSuffixLength,
                    kSuffixLength);
        displayed_path[sizeof(displayed_path) - 1U] = '\0';
    }
    canvas.draw_text(24, 34, displayed_path, 2, GrayLevel::Black);
    canvas.draw_line(24, 76, 776, 76, GrayLevel::Black);
    canvas.draw_text(30, 84, "TYPE / NAME", 1, GrayLevel::Black);
    canvas.draw_text(600, 84, "SIZE", 1, GrayLevel::Black);
    canvas.draw_text(724, 84, "INFO", 1, GrayLevel::Black);

    if (!files.valid) {
        if (files.error == ESP_ERR_NOT_FOUND) {
            canvas.draw_text(30, 122, "NO SD CARD", 3, GrayLevel::Black);
            canvas.draw_text(30, 164,
                             "Insert a card to reload automatically",
                             2, GrayLevel::Black);
        } else {
            char error[64] = {};
            std::snprintf(error, sizeof(error), "Directory error: %s",
                          esp_err_to_name(files.error));
            canvas.draw_text(30, 122, error, 2, GrayLevel::Black);
            canvas.draw_text(30, 162, "Press OK to retry", 2,
                             GrayLevel::Black);
        }
    } else if (files.entry_count == 0) {
        canvas.draw_text(30, 122, "This directory is empty", 2,
                         GrayLevel::Black);
    } else {
        for (size_t row = 0; row < files.entry_count; ++row) {
            draw_list_row(canvas, files, row, state.capacity_unit_mode);
        }
    }

    draw_button(canvas, 96, false);
    draw_button(canvas, 704, true);
    char page[24] = {};
    std::snprintf(page,
                  sizeof(page),
                  "%u / %u",
                  static_cast<unsigned>(files.page_index + 1U),
                  static_cast<unsigned>(files.page_count));
    canvas.draw_text(356, 432, page, 2, GrayLevel::Black);
}
