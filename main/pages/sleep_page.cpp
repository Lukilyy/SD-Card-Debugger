#include "sleep_page.h"

#include "canvas.h"

void sleep_page_render(Canvas &canvas)
{
    canvas.clear(GrayLevel::White);

    constexpr int box_x = 170;
    constexpr int box_y = 105;
    constexpr int box_width = 460;
    constexpr int box_height = 270;

    canvas.draw_rect(box_x, box_y, box_width, box_height, GrayLevel::Black);
    canvas.draw_rect(box_x + 8, box_y + 8,
                     box_width - 16, box_height - 16,
                     GrayLevel::Black);
    canvas.draw_text(250, 180, "Deep Sleep", 4, GrayLevel::Black);
    canvas.draw_line(230, 245, 570, 245, GrayLevel::Black);
    canvas.draw_text(250, 290, "Press AI to wake", 2, GrayLevel::Black);
}
