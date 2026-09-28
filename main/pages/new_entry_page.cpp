#include "new_entry_page.h"

#include <cstdio>
#include <cstring>

#include "app_state.h"
#include "canvas.h"
#include "esp_err.h"
#include "page_controls.h"

namespace {

using page_controls::draw_centered_button;
using page_controls::draw_return_icon;

constexpr int kKeyHeight = 44;
constexpr int kKeyStepY = 48;
constexpr int kKeyboardY = 190;
constexpr int kDeleteX = 650;
constexpr int kDeleteY = kKeyboardY + 3 * kKeyStepY;
constexpr int kDeleteWidth = 110;

struct KeyboardRow {
    const char *characters;
    int x;
    int key_width;
    int key_step;
};

constexpr KeyboardRow kKeyboardRows[] = {
    {"1234567890", 30, 68, 74},
    {"qwertyuiop", 30, 68, 74},
    {"asdfghjkl", 67, 68, 74},
    {"zxcvbnm_-", 40, 60, 66},
};

constexpr const char *kExtensionLabels[] = {
    "TXT", "JSON", "CSV", "MD", "BIN",
};
constexpr const char *kExtensionValues[] = {
    "txt", "json", "csv", "md", "bin",
};
constexpr int kExtensionX = 247;
constexpr int kExtensionY = 152;
constexpr int kExtensionWidth = 94;
constexpr int kExtensionHeight = 32;
constexpr int kExtensionStep = 104;

void draw_keyboard(Canvas &canvas, bool uppercase)
{
    for (size_t row = 0;
         row < sizeof(kKeyboardRows) / sizeof(kKeyboardRows[0]); ++row) {
        const KeyboardRow &layout = kKeyboardRows[row];
        const size_t count = std::strlen(layout.characters);
        for (size_t column = 0; column < count; ++column) {
            const int x = layout.x +
                          static_cast<int>(column) * layout.key_step;
            const int y = kKeyboardY + static_cast<int>(row) * kKeyStepY;
            canvas.draw_rect(x, y, layout.key_width, kKeyHeight,
                             GrayLevel::Black);

            char label[2] = {layout.characters[column], '\0'};
            if (uppercase && label[0] >= 'a' && label[0] <= 'z') {
                label[0] = static_cast<char>(label[0] - 'a' + 'A');
            }
            canvas.draw_text(x + (layout.key_width - 12) / 2,
                             y + 15,
                             label,
                             2,
                             GrayLevel::Black);
        }
    }
    draw_centered_button(canvas, kDeleteX, kDeleteY,
                         kDeleteWidth, kKeyHeight, "DEL");
}

void draw_extension_selector(Canvas &canvas, const AppState &state)
{
    canvas.draw_text(28, 162, "TYPE", 1, GrayLevel::Black);
    const size_t selected =
        static_cast<size_t>(state.create_entry.extension);
    for (size_t index = 0;
         index < sizeof(kExtensionLabels) / sizeof(kExtensionLabels[0]);
         ++index) {
        draw_centered_button(canvas,
                             kExtensionX + static_cast<int>(index) *
                                 kExtensionStep,
                             kExtensionY,
                             kExtensionWidth,
                             kExtensionHeight,
                             kExtensionLabels[index],
                             index == selected);
    }
}

}  // namespace

bool new_entry_back_hit_test(uint16_t x, uint16_t y)
{
    return page_controls::back_hit_test(x, y);
}

bool new_folder_hit_test(uint16_t x, uint16_t y)
{
    return x >= 80 && x < 720 && y >= 128 && y < 268;
}

bool new_file_hit_test(uint16_t x, uint16_t y)
{
    return x >= 80 && x < 720 && y >= 276 && y < 416;
}

char name_input_character_at(uint16_t x, uint16_t y)
{
    if (y < kKeyboardY) {
        return '\0';
    }
    const int row = (y - kKeyboardY) / kKeyStepY;
    const int offset_y = (y - kKeyboardY) % kKeyStepY;
    if (row < 0 || row >= static_cast<int>(
                            sizeof(kKeyboardRows) /
                            sizeof(kKeyboardRows[0])) ||
        offset_y >= kKeyHeight) {
        return '\0';
    }
    const KeyboardRow &layout = kKeyboardRows[row];
    if (x < layout.x) {
        return '\0';
    }
    const int column = (x - layout.x) / layout.key_step;
    const int offset_x = (x - layout.x) % layout.key_step;
    const size_t count = std::strlen(layout.characters);
    return column >= 0 && static_cast<size_t>(column) < count &&
                   offset_x < layout.key_width
               ? layout.characters[column]
               : '\0';
}

int name_input_extension_at(uint16_t x, uint16_t y)
{
    if (x < kExtensionX || y < kExtensionY ||
        y >= kExtensionY + kExtensionHeight) {
        return -1;
    }
    const int index = (x - kExtensionX) / kExtensionStep;
    const int offset = (x - kExtensionX) % kExtensionStep;
    return index >= 0 && index < 5 && offset < kExtensionWidth
               ? index
               : -1;
}

bool name_input_case_hit_test(uint16_t x, uint16_t y)
{
    return x >= 20 && x < 228 && y >= 394 && y < 480;
}

bool name_input_backspace_hit_test(uint16_t x, uint16_t y)
{
    return x >= 638 && x < 780 && y >= 320 && y < 394;
}

bool name_input_cancel_hit_test(uint16_t x, uint16_t y)
{
    return x >= 228 && x < 448 && y >= 394 && y < 480;
}

bool name_input_create_hit_test(uint16_t x, uint16_t y)
{
    return x >= 448 && x < 788 && y >= 394 && y < 480;
}

void new_entry_menu_render(Canvas &canvas)
{
    canvas.clear(GrayLevel::White);
    canvas.draw_text(24, 28, "NEW", 3, GrayLevel::Black);
    draw_return_icon(canvas);
    canvas.draw_line(24, 76, 776, 76, GrayLevel::Black);
    canvas.draw_text(116, 94, "CREATE IN CURRENT DIRECTORY",
                     2, GrayLevel::Black);
    draw_centered_button(canvas, 90, 138, 620, 120, "NEW FOLDER");
    draw_centered_button(canvas, 90, 286, 620, 120, "NEW FILE");
}

void name_input_page_render_value(Canvas &canvas, const AppState &state)
{
    canvas.fill_rect(28, 82, 744, 68, GrayLevel::White);
    canvas.draw_rect(28, 88, 744, 48, GrayLevel::Black);

    char displayed[kCreateEntryNameSize + 6U] = {};
    if (state.create_entry.type == CreateEntryType::File) {
        const size_t extension =
            static_cast<size_t>(state.create_entry.extension);
        std::snprintf(displayed, sizeof(displayed), "%s.%s",
                      state.create_entry.name,
                      extension < 5U ? kExtensionValues[extension] : "txt");
    } else {
        std::snprintf(displayed, sizeof(displayed), "%s",
                      state.create_entry.name);
    }

    constexpr size_t kVisibleCharacters = 55;
    const size_t displayed_length = std::strlen(displayed);
    const char *visible = displayed;
    char shortened[kVisibleCharacters + 4U] = {};
    if (displayed_length > kVisibleCharacters) {
        std::memcpy(shortened, "...", 3U);
        std::memcpy(shortened + 3U,
                    displayed + displayed_length - kVisibleCharacters,
                    kVisibleCharacters);
        shortened[sizeof(shortened) - 1U] = '\0';
        visible = shortened;
    }
    canvas.draw_text(40, 105, visible, 2, GrayLevel::Black);

    if (state.create_entry.error != ESP_OK) {
        const char *message =
            state.create_entry.error == ESP_ERR_INVALID_ARG
                ? "Invalid or empty name"
                : (state.create_entry.error == ESP_ERR_INVALID_STATE
                       ? "Name already exists"
                       : esp_err_to_name(state.create_entry.error));
        canvas.draw_text(32, 139, message, 1, GrayLevel::Black);
    }
}

void name_input_page_render(Canvas &canvas, const AppState &state)
{
    canvas.clear(GrayLevel::White);
    canvas.draw_text(24, 24,
                     state.create_entry.type == CreateEntryType::Folder
                         ? "NEW FOLDER"
                         : "NEW FILE",
                     3,
                     GrayLevel::Black);
    draw_return_icon(canvas);
    canvas.draw_line(24, 76, 776, 76, GrayLevel::Black);
    name_input_page_render_value(canvas, state);
    if (state.create_entry.type == CreateEntryType::File) {
        draw_extension_selector(canvas, state);
    }
    draw_keyboard(canvas, state.create_entry.uppercase);

    draw_centered_button(canvas, 28, 410, 190, 52,
                         state.create_entry.uppercase ? "a-z" : "SHIFT",
                         state.create_entry.uppercase);
    draw_centered_button(canvas, 238, 410, 200, 52, "CANCEL");
    draw_centered_button(canvas, 458, 410, 314, 52, "CREATE", true);
}
