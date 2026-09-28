#pragma once

class Canvas;

// Draws the non-navigable page retained by the e-paper during deep sleep.
void sleep_page_render(Canvas &canvas);
