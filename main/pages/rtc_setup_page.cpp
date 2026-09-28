#include "rtc_setup_page.h"

#include <cstdio>
#include <cstring>

#include "canvas.h"
#include "esp_err.h"
#include "page_controls.h"

namespace {

using page_controls::draw_centered_button;

constexpr int kRowY[] = {92, 150, 208, 266, 324};
constexpr int kRowHeight = 46;
constexpr int kMinusX = 334;
constexpr int kValueX = 430;
constexpr int kValueWidth = 150;
constexpr int kPlusX = 596;
constexpr int kAdjustWidth = 92;
constexpr const char *kLabels[] = {
    "YEAR", "MONTH", "DAY", "HOUR", "MINUTE",
};
constexpr int kNumberKeyX = 190;
constexpr int kNumberKeyY = 142;
constexpr int kNumberKeyWidth = 125;
constexpr int kNumberKeyHeight = 52;
constexpr int kNumberKeyStepX = 145;
constexpr int kNumberKeyStepY = 62;
constexpr char kNumberKeys[] = "123456789";

}  // namespace

bool rtc_setup_adjustment_at(uint16_t x,
                             uint16_t y,
                             DateTimeField &field,
                             int &direction)
{
    for (size_t index = 0; index < 5U; ++index) {
        if (y < kRowY[index] - 5 || y >= kRowY[index] + kRowHeight + 5) {
            continue;
        }
        if (x >= kMinusX - 8 && x < kMinusX + kAdjustWidth + 8) {
            field = static_cast<DateTimeField>(index);
            direction = -1;
            return true;
        }
        if (x >= kPlusX - 8 && x < kPlusX + kAdjustWidth + 8) {
            field = static_cast<DateTimeField>(index);
            direction = 1;
            return true;
        }
    }
    return false;
}

bool rtc_setup_value_field_at(uint16_t x,
                              uint16_t y,
                              DateTimeField &field)
{
    if (x < kValueX - 8 || x >= kValueX + kValueWidth + 8) {
        return false;
    }
    for (size_t index = 0; index < 5U; ++index) {
        if (y >= kRowY[index] - 5 &&
            y < kRowY[index] + kRowHeight + 5) {
            field = static_cast<DateTimeField>(index);
            return true;
        }
    }
    return false;
}

bool rtc_setup_cancel_hit_test(uint16_t x, uint16_t y)
{
    return x >= 44 && x < 390 && y >= 392 && y < 480;
}

bool rtc_setup_save_hit_test(uint16_t x, uint16_t y)
{
    return x >= 410 && x < 756 && y >= 392 && y < 480;
}

void rtc_setup_page_render(Canvas &canvas, const AppState &state)
{
    canvas.clear(GrayLevel::White);
    canvas.draw_text(24, 24, "SET DATE & TIME", 3, GrayLevel::Black);
    canvas.draw_text(548, 44, "SECONDS: 00", 1, GrayLevel::Black);
    canvas.draw_line(24, 76, 776, 76, GrayLevel::Black);
    canvas.draw_text(62, 80, "RTC INVALID - CONFIRM LOCAL TIME",
                     1, GrayLevel::Black);

    const RtcDateTime &value = state.rtc_setup.value;
    const int values[] = {
        value.year, value.month, value.day, value.hour, value.minute,
    };
    for (size_t index = 0; index < 5U; ++index) {
        canvas.draw_text(62, kRowY[index] + 15,
                         kLabels[index], 2, GrayLevel::Black);
        draw_centered_button(canvas, kMinusX, kRowY[index],
                             kAdjustWidth, kRowHeight, "-");

        char displayed[16] = {};
        if (index == 0U) {
            std::snprintf(displayed, sizeof(displayed), "%04d", values[index]);
        } else {
            std::snprintf(displayed, sizeof(displayed), "%02d", values[index]);
        }
        canvas.draw_rect(kValueX, kRowY[index],
                         kValueWidth, kRowHeight, GrayLevel::Black);
        canvas.draw_text(kValueX + (kValueWidth -
                         static_cast<int>(std::strlen(displayed)) * 24) / 2,
                         kRowY[index] + 10,
                         displayed,
                         3,
                         GrayLevel::Black);
        draw_centered_button(canvas, kPlusX, kRowY[index],
                             kAdjustWidth, kRowHeight, "+");
    }

    if (state.rtc_setup.error != ESP_OK) {
        canvas.draw_text(62, 382, esp_err_to_name(state.rtc_setup.error),
                         1, GrayLevel::Black);
    }
    draw_centered_button(canvas, 54, 410, 326, 52, "CANCEL");
    draw_centered_button(canvas, 420, 410, 326, 52, "SAVE", true);
}

char rtc_number_input_digit_at(uint16_t x, uint16_t y)
{
    if (x < kNumberKeyX || y < kNumberKeyY) {
        return '\0';
    }
    const int column = (x - kNumberKeyX) / kNumberKeyStepX;
    const int row = (y - kNumberKeyY) / kNumberKeyStepY;
    const int offset_x = (x - kNumberKeyX) % kNumberKeyStepX;
    const int offset_y = (y - kNumberKeyY) % kNumberKeyStepY;
    if (column < 0 || column >= 3 || row < 0 || row >= 4 ||
        offset_x >= kNumberKeyWidth || offset_y >= kNumberKeyHeight) {
        return '\0';
    }
    if (row < 3) {
        return kNumberKeys[row * 3 + column];
    }
    return column == 1 ? '0' : '\0';
}

bool rtc_number_input_delete_hit_test(uint16_t x, uint16_t y)
{
    return x >= kNumberKeyX - 8 &&
           x < kNumberKeyX + kNumberKeyWidth + 8 &&
           y >= kNumberKeyY + 3 * kNumberKeyStepY - 5 &&
           y < kNumberKeyY + 3 * kNumberKeyStepY + kNumberKeyHeight + 5;
}

bool rtc_number_input_ok_hit_test(uint16_t x, uint16_t y)
{
    const int key_x = kNumberKeyX + 2 * kNumberKeyStepX;
    return x >= key_x - 8 && x < key_x + kNumberKeyWidth + 8 &&
           y >= kNumberKeyY + 3 * kNumberKeyStepY - 5 &&
           y < kNumberKeyY + 3 * kNumberKeyStepY + kNumberKeyHeight + 5;
}

bool rtc_number_input_cancel_hit_test(uint16_t x, uint16_t y)
{
    return x >= 180 && x < 615 && y >= 398 && y < 480;
}

void rtc_number_input_page_render_value(Canvas &canvas, const AppState &state)
{
    const DateTimeNumberInputState &input = state.date_time_input;
    canvas.fill_rect(190, 84, 415, 46, GrayLevel::White);
    canvas.draw_rect(190, 84, 415, 46, GrayLevel::Black);
    const char *displayed = input.length > 0U ? input.digits : "_";
    const int text_width = static_cast<int>(std::strlen(displayed)) * 24;
    canvas.draw_text(190 + (415 - text_width) / 2,
                     94, displayed, 3, GrayLevel::Black);
    canvas.fill_rect(612, 100, 164, 22, GrayLevel::White);
    if (input.error != ESP_OK) {
        canvas.draw_text(628, 108, "INVALID VALUE", 1, GrayLevel::Black);
    }
}

void rtc_number_input_page_render(Canvas &canvas, const AppState &state)
{
    const DateTimeNumberInputState &input = state.date_time_input;
    const size_t field_index = static_cast<size_t>(input.field);
    const char *field_label = field_index < 5U ? kLabels[field_index] : "VALUE";

    canvas.clear(GrayLevel::White);
    canvas.draw_text(24, 24, "ENTER NUMBER", 3, GrayLevel::Black);
    canvas.draw_text(602, 44, field_label, 1, GrayLevel::Black);
    canvas.draw_line(24, 76, 776, 76, GrayLevel::Black);
    rtc_number_input_page_render_value(canvas, state);

    for (size_t index = 0; index < sizeof(kNumberKeys) - 1U; ++index) {
        const int column = static_cast<int>(index % 3U);
        const int row = static_cast<int>(index / 3U);
        char label[2] = {kNumberKeys[index], '\0'};
        draw_centered_button(canvas,
                             kNumberKeyX + column * kNumberKeyStepX,
                             kNumberKeyY + row * kNumberKeyStepY,
                             kNumberKeyWidth,
                             kNumberKeyHeight,
                             label);
    }
    const int last_row_y = kNumberKeyY + 3 * kNumberKeyStepY;
    draw_centered_button(canvas, kNumberKeyX, last_row_y,
                         kNumberKeyWidth, kNumberKeyHeight, "DEL");
    draw_centered_button(canvas, kNumberKeyX + kNumberKeyStepX, last_row_y,
                         kNumberKeyWidth, kNumberKeyHeight, "0");
    draw_centered_button(canvas, kNumberKeyX + 2 * kNumberKeyStepX, last_row_y,
                         kNumberKeyWidth, kNumberKeyHeight, "OK", true);

    draw_centered_button(canvas, 190, 410, 415, 50, "CANCEL");
}
