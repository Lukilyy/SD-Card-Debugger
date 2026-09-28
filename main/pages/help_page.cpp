#include "help_page.h"

#include <cstdio>
#include <cstring>

#include "app_state.h"
#include "canvas.h"
#include "page_controls.h"

namespace {

using page_controls::draw_return_icon;

void draw_battery_status(Canvas &canvas, const BatteryStatusState &battery)
{
    constexpr int kScreenWidth = 800;
    constexpr int kBatteryY = 24;
    constexpr int kBatteryWidth = 32;
    constexpr int kBatteryHeight = 20;
    constexpr int kBatteryTerminalWidth = 4;
    constexpr int kStatusGap = 14;
    constexpr int kBoltWidth = 5;
    constexpr int kBoltGap = 10;

    char percent[8] = "--%";
    if (battery.valid) {
        std::snprintf(percent, sizeof(percent), "%d%%", battery.percent);
    }
    const int percent_width = static_cast<int>(std::strlen(percent)) * 12;
    const bool show_bolt = battery.charging_valid && battery.charging;
    const int bolt_section_width = show_bolt ? kBoltWidth + kBoltGap : 0;
    const int total_width = bolt_section_width + kBatteryWidth +
                            kBatteryTerminalWidth + kStatusGap + percent_width;
    const int start_x = (kScreenWidth - total_width) / 2;
    const int battery_x = start_x + bolt_section_width;

    canvas.draw_rect(battery_x, kBatteryY,
                     kBatteryWidth, kBatteryHeight, GrayLevel::Black);
    canvas.draw_rect(battery_x + kBatteryWidth, kBatteryY + 6,
                     4, 8, GrayLevel::Black);

    if (battery.valid) {
        const int fill_width =
            battery.percent * (kBatteryWidth - 6) / 100;
        canvas.fill_rect(battery_x + 3, kBatteryY + 3,
                         fill_width, kBatteryHeight - 6,
                         GrayLevel::Black);
    }

    if (show_bolt) {
        static constexpr uint8_t kBolt[13] = {
            0x06, 0x06, 0x0C, 0x0C, 0x18, 0x1F, 0x0E,
            0x06, 0x06, 0x0C, 0x0C, 0x08, 0x08,
        };
        for (int row = 0; row < 13; ++row) {
            for (int column = 0; column < 5; ++column) {
                if ((kBolt[row] & (1U << (4 - column))) != 0U) {
                    canvas.draw_pixel(start_x + column, 27 + row,
                                      GrayLevel::Black);
                }
            }
        }
    }

    canvas.draw_text(battery_x + kBatteryWidth + kBatteryTerminalWidth +
                         kStatusGap,
                     27, percent, 2, GrayLevel::Black);
}

void draw_tab(Canvas &canvas,
              int x,
              int width,
              const char *label,
              bool selected)
{
    const GrayLevel background = selected ? GrayLevel::Black
                                          : GrayLevel::White;
    const GrayLevel foreground = selected ? GrayLevel::White
                                          : GrayLevel::Black;
    canvas.fill_rect(x, 82, width, 44, background);
    canvas.draw_rect(x, 82, width, 44, GrayLevel::Black);
    const int text_width = static_cast<int>(std::strlen(label)) * 12;
    canvas.draw_text(x + (width - text_width) / 2, 97,
                     label, 2, foreground);
}

void draw_help_row(Canvas &canvas,
                   int y,
                   const char *label,
                   const char *description)
{
    canvas.draw_text(34, y, label, 2, GrayLevel::Black);
    canvas.draw_text(198, y, description, 2, GrayLevel::Black);
    canvas.draw_line(34, y + 25, 766, y + 25, GrayLevel::Black);
}

void draw_about(Canvas &canvas)
{
    canvas.draw_text(34, 146, "SD CARD DEBUGGER", 3, GrayLevel::Black);
    canvas.draw_text(34, 181,
                     "Inspect and manage FAT32 / exFAT cards on Sticky.",
                     2, GrayLevel::Black);

    draw_help_row(canvas, 224, "SD INFO",
                  "Capacity, usage, filesystem and layout");
    draw_help_row(canvas, 260, "FILES",
                  "Browse, create and delete files or folders");
    draw_help_row(canvas, 296, "DIAGNOSTICS",
                  "Read-only MBR, GPT and filesystem checks");
    draw_help_row(canvas, 332, "STORAGE TEST",
                  "Write, read and verify card storage");
    draw_help_row(canvas, 368, "CARD TOOLS",
                  "Clear content or rebuild an unusable card");

    canvas.draw_text(34, 421,
                     "Raw card details remain available if mount fails.",
                     2, GrayLevel::Black);
}

void draw_guide(Canvas &canvas)
{
    draw_help_row(canvas, 142, "SD INFO",
                  "READ ONLY; check capacity and mount status.");
    draw_help_row(canvas, 180, "FILES",
                  "Browse only reads; create/delete changes data.");
    draw_help_row(canvas, 218, "DIAGNOSTICS",
                  "READ ONLY; use first when mount fails.");
    draw_help_row(canvas, 256, "STORAGE TEST",
                  "Writes and removes one temporary test file.");
    draw_help_row(canvas, 294, "CLEAR CARD",
                  "Deletes every file and folder on the card.");
    draw_help_row(canvas, 332, "INITIALIZE",
                  "Erases partitions; creates MBR + FAT32/exFAT.");

    canvas.draw_rect(34, 370, 732, 76, GrayLevel::Black);
    canvas.draw_text(54, 386,
                     "CLEAR keeps the current partition and filesystem.",
                     2, GrayLevel::Black);
    canvas.draw_text(54, 418,
                     "INITIALIZE rebuilds and reformats the whole card.",
                     2, GrayLevel::Black);
}

}  // namespace

bool help_page_back_hit_test(uint16_t x, uint16_t y)
{
    return page_controls::back_hit_test(x, y);
}

bool help_page_about_hit_test(uint16_t x, uint16_t y)
{
    return x >= 174 && x < 390 && y >= 76 && y < 134;
}

bool help_page_guide_hit_test(uint16_t x, uint16_t y)
{
    return x >= 410 && x < 626 && y >= 76 && y < 134;
}

void help_page_render_battery_status(Canvas &canvas, const AppState &state)
{
    canvas.fill_rect(280, 12, 240, 44, GrayLevel::White);
    draw_battery_status(canvas, state.battery);
}

void help_page_render(Canvas &canvas, const AppState &state)
{
    canvas.clear(GrayLevel::White);
    canvas.draw_text(24, 24, "HELP", 3, GrayLevel::Black);
    help_page_render_battery_status(canvas, state);
    draw_return_icon(canvas);
    canvas.draw_line(24, 70, 776, 70, GrayLevel::Black);

    draw_tab(canvas, 180, 200, "ABOUT",
             state.help_section == HelpSection::About);
    draw_tab(canvas, 420, 200, "HOW TO USE",
             state.help_section == HelpSection::Guide);

    if (state.help_section == HelpSection::About) {
        draw_about(canvas);
    } else {
        draw_guide(canvas);
    }
}
