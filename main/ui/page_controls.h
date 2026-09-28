#pragma once

#include <cstdint>
#include <cstring>

#include "canvas.h"

namespace page_controls {

// Shared drawing and touch geometry for the existing 800 x 480 page controls.
inline void draw_three_pixel_diagonal(Canvas &canvas,
                                      int start_x,
                                      int start_y,
                                      int step_x,
                                      int step_y,
                                      int steps)
{
    for (int i = 0; i <= steps; ++i) {
        canvas.fill_rect(start_x + i * step_x - 1,
                         start_y + i * step_y - 1,
                         3,
                         3,
                         GrayLevel::Black);
    }
}

inline void draw_return_icon(Canvas &canvas)
{
    canvas.fill_rect(739, 35, 22, 3, GrayLevel::Black);
    draw_three_pixel_diagonal(canvas, 739, 36, 1, -1, 8);
    draw_three_pixel_diagonal(canvas, 739, 36, 1, 1, 8);
    canvas.fill_rect(753, 48, 9, 3, GrayLevel::Black);
    canvas.fill_rect(759, 48, 3, 7, GrayLevel::Black);
}

inline bool back_hit_test(uint16_t x, uint16_t y)
{
    return x >= 700 && x < 780 && y >= 12 && y < 72;
}

inline bool previous_page_hit_test(uint16_t x, uint16_t y)
{
    return x >= 16 && x < 176 && y >= 400 && y < 480;
}

inline bool next_page_hit_test(uint16_t x, uint16_t y)
{
    return x >= 624 && x < 784 && y >= 400 && y < 480;
}

inline void draw_centered_button(Canvas &canvas,
                                 int x,
                                 int y,
                                 int width,
                                 int height,
                                 const char *label,
                                 bool filled = false)
{
    if (filled) {
        canvas.fill_rect(x, y, width, height, GrayLevel::Black);
    } else {
        canvas.draw_rect(x, y, width, height, GrayLevel::Black);
    }
    const GrayLevel color = filled ? GrayLevel::White : GrayLevel::Black;
    const int text_width = static_cast<int>(std::strlen(label)) * 12;
    canvas.draw_text(x + (width - text_width) / 2,
                     y + (height - 14) / 2,
                     label,
                     2,
                     color);
}

}  // namespace page_controls
