#include "home_page.h"

#include <cstdio>
#include <cstring>

#include "app_state.h"
#include "capacity_format.h"
#include "canvas.h"
#include "esp_err.h"

namespace {

constexpr uint16_t kFooterHitTop = 397;
constexpr uint16_t kFooterHitBottom = 462;

void draw_row(Canvas &canvas,
              int y,
              const char *label,
              const char *value)
{
    canvas.draw_text(54, y, label, 2, GrayLevel::Black);
    canvas.draw_text(310, y, value, 2, GrayLevel::Black);
    canvas.draw_line(54, y + 23, 746, y + 23, GrayLevel::Black);
}

void draw_footer_button(Canvas &canvas,
                        int x,
                        int width,
                        const char *label)
{
    canvas.draw_rect(x, 411, width, 42, GrayLevel::Black);
    constexpr int kScale = 2;
    constexpr int kGlyphAdvance = 6 * kScale;
    constexpr int kTextHeight = 7 * kScale;
    const int text_width =
        static_cast<int>(std::strlen(label)) * kGlyphAdvance;
    const int text_x = x + (width - text_width) / 2;
    const int text_y = 411 + (42 - kTextHeight) / 2;
    canvas.draw_text(text_x, text_y, label, kScale, GrayLevel::Black);
}

void draw_capacity_units_toggle(Canvas &canvas, CapacityUnitMode mode)
{
    constexpr int kX = 458;
    constexpr int kY = 30;
    constexpr int kSegmentWidth = 58;
    constexpr int kHeight = 30;
    const bool decimal = mode == CapacityUnitMode::Decimal;
    canvas.fill_rect(kX, kY, kSegmentWidth, kHeight,
                     decimal ? GrayLevel::Black : GrayLevel::White);
    canvas.fill_rect(kX + kSegmentWidth, kY, kSegmentWidth, kHeight,
                     decimal ? GrayLevel::White : GrayLevel::Black);
    canvas.draw_rect(kX, kY, kSegmentWidth * 2, kHeight,
                     GrayLevel::Black);
    canvas.draw_line(kX + kSegmentWidth, kY,
                     kX + kSegmentWidth, kY + kHeight,
                     GrayLevel::Black);
    canvas.draw_text(kX + 17, kY + 8, "GB", 2,
                     decimal ? GrayLevel::White : GrayLevel::Black);
    canvas.draw_text(kX + kSegmentWidth + 11, kY + 8, "GiB", 2,
                     decimal ? GrayLevel::Black : GrayLevel::White);
}

}  // namespace

bool home_page_files_hit_test(uint16_t x, uint16_t y)
{
    return x >= 424 && x < 572 &&
           y >= kFooterHitTop && y < kFooterHitBottom;
}

bool home_page_help_hit_test(uint16_t x, uint16_t y)
{
    return x >= 258 && x < 406 &&
           y >= kFooterHitTop && y < kFooterHitBottom;
}

bool home_page_tools_hit_test(uint16_t x, uint16_t y)
{
    return x >= 590 && x < 738 &&
           y >= kFooterHitTop && y < kFooterHitBottom;
}

bool home_page_capacity_units_hit_test(uint16_t x, uint16_t y)
{
    return x >= 446 && x < 586 && y >= 16 && y < 72;
}

void home_page_render(Canvas &canvas, const AppState &state)
{
    const StickySdCardInfo &card = state.sd_card;
    canvas.clear(GrayLevel::White);
    canvas.draw_rect(20, 18, 760, 444, GrayLevel::Black);

    canvas.draw_text(46, 38, "SD CARD DEBUGGER", 3, GrayLevel::Black);
    draw_capacity_units_toggle(canvas, state.capacity_unit_mode);
    const char *headline = card.mounted
                               ? "[ READY ]"
                               : (card.raw_readable ? "[ RAW OK ]"
                                                    : "[ ERROR ]");
    const int headline_width = static_cast<int>(std::strlen(headline)) * 12;
    canvas.draw_text(754 - headline_width, 42, headline,
                     2, GrayLevel::Black);
    canvas.draw_line(44, 73, 756, 73, GrayLevel::Black);

    char total[24] = "--";
    char used[24] = "--";
    char free_space[24] = "--";
    char sector[40] = "--";
    char cluster[24] = "--";
    char status[48] = {};

    if (card.mounted || card.raw_readable) {
        format_capacity(card.total_bytes, state.capacity_unit_mode,
                        total, sizeof(total));
        if (card.sector_size != 0U && card.sector_count != 0U) {
            std::snprintf(sector, sizeof(sector), "%lu B / %llu",
                          static_cast<unsigned long>(card.sector_size),
                          static_cast<unsigned long long>(card.sector_count));
        }
        if (card.cluster_size != 0U) {
            format_capacity(card.cluster_size, state.capacity_unit_mode,
                            cluster, sizeof(cluster));
        }
    }

    if (card.mounted) {
        format_capacity(card.used_bytes, state.capacity_unit_mode,
                        used, sizeof(used));
        format_capacity(card.free_bytes, state.capacity_unit_mode,
                        free_space, sizeof(free_space));
        std::snprintf(status, sizeof(status), "Mounted (%s)", card.card_type);
    } else if (!card.inserted) {
        std::snprintf(status, sizeof(status), "No card detected");
    } else if (card.raw_readable) {
        std::snprintf(status, sizeof(status),
                      "Card readable; mount failed");
    } else {
        std::snprintf(status, sizeof(status), "Mount failed: %s",
                      esp_err_to_name(card.error));
    }

    draw_row(canvas, 88, "STATUS", status);
    draw_row(canvas, 123, "TOTAL", total);
    draw_row(canvas, 158, "USED", used);
    draw_row(canvas, 193, "FREE", free_space);
    draw_row(canvas, 228, "FILE SYSTEM",
             std::strcmp(card.file_system, "UNKNOWN") != 0
                 ? card.file_system : "--");
    draw_row(canvas, 263, "PARTITION",
             std::strcmp(card.partition, "UNKNOWN") != 0
                 ? card.partition : "--");
    draw_row(canvas, 298, "SECTOR", sector);
    draw_row(canvas, 333, "CLUSTER", cluster);

    canvas.draw_text(54, 374,
                     card.inserted && !card.mounted
                         ? "TOOLS: DIAGNOSTICS / INITIALIZE"
                         : "OK: REFRESH",
                     2, GrayLevel::Black);
    draw_footer_button(canvas, 258, 148, "HELP");
    draw_footer_button(canvas, 424, 148, "FILES");
    draw_footer_button(canvas, 590, 148, "TOOLS");
}
