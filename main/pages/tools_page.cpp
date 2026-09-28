#include "tools_page.h"

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

void draw_header(Canvas &canvas, const char *title)
{
    canvas.draw_text(28, 28, title, 3, GrayLevel::Black);
    draw_return_icon(canvas);
    canvas.draw_line(28, 72, 772, 72, GrayLevel::Black);
}

void draw_value(Canvas &canvas, int y, const char *label, const char *value)
{
    canvas.draw_text(42, y, label, 2, GrayLevel::Black);
    canvas.draw_text(270, y, value, 2, GrayLevel::Black);
    canvas.draw_line(42, y + 22, 758, y + 22, GrayLevel::Black);
}

const char *scheme_name(StickySdPartitionScheme scheme)
{
    switch (scheme) {
    case StickySdPartitionScheme::Mbr: return "MBR";
    case StickySdPartitionScheme::Gpt: return "GPT";
    case StickySdPartitionScheme::Superfloppy: return "SUPERFLOPPY";
    case StickySdPartitionScheme::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

const char *valid_text(bool value) { return value ? "VALID" : "INVALID"; }

bool has_invalid_partition(const StickySdRawDiagnostics &data)
{
    for (size_t index = 0; index < data.detected_partition_count; ++index) {
        if (!data.detected_partitions[index].range_valid) return true;
    }
    return false;
}

bool has_known_unsupported_partition(const StickySdRawDiagnostics &data)
{
    for (size_t index = 0; index < data.detected_partition_count; ++index) {
        const StickySdDetectedPartition &partition =
            data.detected_partitions[index];
        if (partition.mbr_type == 0x83 ||
            std::strcmp(partition.partition_type, "LINUX FS") == 0) {
            return true;
        }
    }
    return false;
}

const char *diagnostic_status(const StickySdRawDiagnostics &data)
{
    if (!data.inserted) return "NO CARD";
    if (!data.card_initialized) return "CARD ACCESS ERROR";
    if (!data.sector0_read || data.error != ESP_OK) return "READ ERROR";
    if (data.scheme == StickySdPartitionScheme::Gpt && !data.gpt_valid)
        return "GPT DAMAGED";
    if (data.scheme == StickySdPartitionScheme::Unknown ||
        data.total_detected_partition_count == 0U)
        return "NO VALID PARTITION";
    if (has_invalid_partition(data)) return "PARTITION DAMAGED";
    if (std::strcmp(data.detected_file_system, "NTFS") == 0)
        return "UNSUPPORTED FILESYSTEM";
    if (has_known_unsupported_partition(data))
        return "UNSUPPORTED FILESYSTEM";
    if (std::strcmp(data.detected_file_system, "UNKNOWN") == 0)
        return "FILESYSTEM NOT FOUND";
    if (!data.mount_ok) return "MOUNT FAILED";
    return "CARD OK";
}

const char *diagnostic_problem(const StickySdRawDiagnostics &data)
{
    const char *status = diagnostic_status(data);
    if (std::strcmp(status, "CARD OK") == 0)
        return "Card and filesystem are ready to use";
    if (std::strcmp(status, "NO CARD") == 0)
        return "No MicroSD card is inserted";
    if (std::strcmp(status, "CARD ACCESS ERROR") == 0)
        return "Card did not respond over SPI";
    if (std::strcmp(status, "READ ERROR") == 0)
        return "Card responded, but raw sector read failed";
    if (std::strcmp(status, "GPT DAMAGED") == 0)
        return "GPT header or partition-table CRC is invalid";
    if (std::strcmp(status, "NO VALID PARTITION") == 0) {
        return data.scheme == StickySdPartitionScheme::Gpt
                   ? "GPT is valid but has no usable partition"
                   : "No usable data partition was found";
    }
    if (std::strcmp(status, "PARTITION DAMAGED") == 0)
        return "A partition extends beyond the card capacity";
    if (std::strcmp(status, "UNSUPPORTED FILESYSTEM") == 0)
        return "The detected filesystem cannot be mounted here";
    if (std::strcmp(status, "FILESYSTEM NOT FOUND") == 0) {
        return data.scheme == StickySdPartitionScheme::Gpt
                   ? "GPT exists; no FAT32/exFAT volume was found"
                   : "Partition exists; FAT32/exFAT was not found";
    }
    if (data.scheme == StickySdPartitionScheme::Gpt)
        return "Filesystem found; GPT layout cannot mount here";
    return "FAT32/exFAT found, but mount was rejected";
}

const char *diagnostic_action(const StickySdRawDiagnostics &data)
{
    const char *status = diagnostic_status(data);
    if (std::strcmp(status, "CARD OK") == 0) return "No action is needed";
    if (std::strcmp(status, "NO CARD") == 0)
        return "Insert a card, then run diagnostics again";
    if (std::strcmp(status, "CARD ACCESS ERROR") == 0 ||
        std::strcmp(status, "READ ERROR") == 0)
        return "Reinsert the card and check its contacts";
    if (std::strcmp(status, "GPT DAMAGED") == 0)
        return "Back up on a PC; initialize only if needed";
    if (std::strcmp(status, "NO VALID PARTITION") == 0)
        return "Back up if needed, then initialize the card";
    if (std::strcmp(status, "PARTITION DAMAGED") == 0)
        return "Back up on a PC before initializing";
    if (std::strcmp(status, "UNSUPPORTED FILESYSTEM") == 0)
        return "Back up on a PC; use FAT32 or exFAT";
    if (std::strcmp(status, "FILESYSTEM NOT FOUND") == 0)
        return "Check on a PC; initialize if data is not needed";
    if (data.scheme == StickySdPartitionScheme::Gpt)
        return "Back up data; initialize as MBR if desired";
    return "Check DETAILS; back up before initializing";
}

void draw_page_arrow(Canvas &canvas, int x, bool left)
{
    const int center_y = 439;
    if (!left) {
        canvas.fill_rect(x - 13, center_y - 1, 12, 3,
                         GrayLevel::Black);
        draw_three_pixel_diagonal(canvas, x + 12, center_y,
                                  -1, -1, 7);
        draw_three_pixel_diagonal(canvas, x + 12, center_y,
                                  -1, 1, 7);
    } else {
        canvas.fill_rect(x + 2, center_y - 1, 12, 3,
                         GrayLevel::Black);
        draw_three_pixel_diagonal(canvas, x - 12, center_y,
                                  1, -1, 7);
        draw_three_pixel_diagonal(canvas, x - 12, center_y,
                                  1, 1, 7);
    }
}

bool diagnostics_has_partition_pages(const StickySdRawDiagnostics &data)
{
    return data.detected_partition_count > 0U &&
           (data.scheme == StickySdPartitionScheme::Gpt ||
            data.detected_partition_count > 1U);
}

void format_partition_type(const StickySdDetectedPartition &partition,
                           char *output,
                           size_t output_size)
{
    if (partition.from_gpt || partition.mbr_type == 0U) {
        std::snprintf(output, output_size, "%s",
                      partition.partition_type);
    } else {
        std::snprintf(output, output_size, "0x%02X",
                      static_cast<unsigned>(partition.mbr_type));
    }
}

void draw_single_partition_details(
    Canvas &canvas,
    const StickySdDetectedPartition &partition,
    CapacityUnitMode unit_mode)
{
    char heading[32] = {};
    char type[24] = {};
    char start[24] = {};
    char sectors[24] = {};
    char cluster[24] = "--";
    std::snprintf(heading, sizeof(heading), "PARTITION %lu",
                  static_cast<unsigned long>(partition.table_index));
    format_partition_type(partition, type, sizeof(type));
    std::snprintf(start, sizeof(start), "%llu",
                  static_cast<unsigned long long>(partition.start_lba));
    std::snprintf(sectors, sizeof(sectors), "%llu",
                  static_cast<unsigned long long>(partition.sector_count));
    if (partition.cluster_size != 0U) {
        format_capacity(partition.cluster_size, unit_mode,
                        cluster, sizeof(cluster));
    }

    canvas.draw_text(42, 238, heading, 2, GrayLevel::Black);
    draw_value(canvas, 263, "TYPE", type);
    draw_value(canvas, 291, "START LBA", start);
    draw_value(canvas, 319, "SECTORS", sectors);
    draw_value(canvas, 347, "FILESYSTEM", partition.file_system);
    draw_value(canvas, 375, "CLUSTER", cluster);
}

void draw_partition_record(Canvas &canvas,
                           int y,
                           const StickySdDetectedPartition &partition,
                           CapacityUnitMode unit_mode)
{
    char heading[40] = {};
    char type[32] = {};
    char range[72] = {};
    char file_system[72] = {};
    format_partition_type(partition, type, sizeof(type));
    std::snprintf(heading, sizeof(heading), "%s PARTITION %lu",
                  partition.from_gpt ? "GPT" : "MBR",
                  static_cast<unsigned long>(partition.table_index));
    std::snprintf(range, sizeof(range), "START %llu  SECTORS %llu",
                  static_cast<unsigned long long>(partition.start_lba),
                  static_cast<unsigned long long>(partition.sector_count));
    if (partition.cluster_size != 0U) {
        char cluster[24] = {};
        format_capacity(partition.cluster_size, unit_mode,
                        cluster, sizeof(cluster));
        std::snprintf(file_system, sizeof(file_system),
                      "FILESYSTEM %s  CLUSTER %s",
                      partition.file_system, cluster);
    } else {
        std::snprintf(file_system, sizeof(file_system), "FILESYSTEM %s",
                      partition.file_system);
    }

    canvas.draw_text(42, y, heading, 2, GrayLevel::Black);
    canvas.draw_text(270, y, type, 2, GrayLevel::Black);
    canvas.draw_text(42, y + 28, range, 2, GrayLevel::Black);
    canvas.draw_text(42, y + 56, file_system, 2, GrayLevel::Black);
    canvas.draw_line(42, y + 79, 758, y + 79, GrayLevel::Black);
}

}  // namespace

bool tools_page_back_hit_test(uint16_t x, uint16_t y)
{
    return page_controls::back_hit_test(x, y);
}

bool tools_page_diagnostics_hit_test(uint16_t x, uint16_t y)
{
    return x >= 90 && x < 710 && y >= 105 && y < 175;
}

bool tools_page_storage_test_hit_test(uint16_t x, uint16_t y)
{
    return x >= 90 && x < 710 && y >= 187 && y < 257;
}

bool tools_page_clear_card_hit_test(uint16_t x, uint16_t y)
{
    return x >= 90 && x < 710 && y >= 269 && y < 339;
}

bool tools_page_initialize_card_hit_test(uint16_t x, uint16_t y)
{
    return x >= 90 && x < 710 && y >= 351 && y < 421;
}

bool diagnostics_page_back_hit_test(uint16_t x, uint16_t y)
{
    return tools_page_back_hit_test(x, y);
}

bool diagnostics_page_prev_hit_test(uint16_t x, uint16_t y)
{
    return page_controls::previous_page_hit_test(x, y);
}

bool diagnostics_page_next_hit_test(uint16_t x, uint16_t y)
{
    return page_controls::next_page_hit_test(x, y);
}

size_t diagnostics_page_count(const AppState &state)
{
    constexpr size_t kPartitionsPerPage = 3;
    if (!diagnostics_has_partition_pages(state.diagnostics)) {
        return 2U;
    }
    const size_t partition_pages =
        (state.diagnostics.detected_partition_count +
         kPartitionsPerPage - 1U) / kPartitionsPerPage;
    return 2U + partition_pages;
}

bool storage_test_page_back_hit_test(uint16_t x, uint16_t y)
{
    return tools_page_back_hit_test(x, y);
}

bool storage_test_page_start_hit_test(uint16_t x, uint16_t y)
{
    return x >= 254 && x < 546 && y >= 390 && y < 456;
}

bool clear_card_page_back_hit_test(uint16_t x, uint16_t y)
{
    return tools_page_back_hit_test(x, y);
}

bool clear_card_page_action_hit_test(uint16_t x, uint16_t y)
{
    return x >= 254 && x < 546 && y >= 390 && y < 456;
}

bool clear_card_confirm_cancel_hit_test(uint16_t x, uint16_t y)
{
    return x >= 140 && x < 370 && y >= 388 && y < 458;
}

bool clear_card_confirm_clear_hit_test(uint16_t x, uint16_t y)
{
    return x >= 430 && x < 680 && y >= 388 && y < 458;
}

bool initialize_card_page_back_hit_test(uint16_t x, uint16_t y)
{
    return tools_page_back_hit_test(x, y);
}

bool initialize_card_page_action_hit_test(uint16_t x, uint16_t y)
{
    return x >= 254 && x < 546 && y >= 390 && y < 456;
}

bool initialize_card_confirm_cancel_hit_test(uint16_t x, uint16_t y)
{
    return x >= 140 && x < 370 && y >= 388 && y < 458;
}

bool initialize_card_confirm_start_hit_test(uint16_t x, uint16_t y)
{
    return x >= 430 && x < 680 && y >= 388 && y < 458;
}

void tools_page_render(Canvas &canvas)
{
    canvas.clear(GrayLevel::White);
    draw_header(canvas, "TOOLS");
    constexpr const char *labels[] = {
        "DIAGNOSTICS", "STORAGE TEST", "CLEAR CARD", "INITIALIZE CARD",
    };
    for (size_t index = 0; index < 4; ++index) {
        const int y = 105 + static_cast<int>(index) * 82;
        canvas.draw_rect(90, y, 620, 70, GrayLevel::Black);
        canvas.draw_text(120, y + 24, labels[index], 2, GrayLevel::Black);
        canvas.draw_text(650, y + 24, ">", 2, GrayLevel::Black);
    }
}

void clear_card_page_render(Canvas &canvas, const AppState &state)
{
    const StickySdClearCardResult &result = state.clear_card;
    canvas.clear(GrayLevel::White);
    draw_header(canvas, "CLEAR CARD");

    if (!result.attempted) {
        canvas.draw_rect(42, 96, 716, 62, GrayLevel::Black);
        canvas.draw_text(64, 115, "WARNING: DESTRUCTIVE ACTION", 3,
                         GrayLevel::Black);
        canvas.draw_text(54, 196,
                         "Only files and folders on this card are deleted.",
                         2, GrayLevel::Black);
        canvas.draw_text(54, 232,
                         "Partition and filesystem format stay unchanged.",
                         2, GrayLevel::Black);
        canvas.draw_text(54, 282,
                         "This action cannot be undone.",
                         2, GrayLevel::Black);
    } else {
        canvas.draw_rect(42, 96, 716, 62, GrayLevel::Black);
        canvas.draw_text(64, 115,
                         result.passed ? "CARD CLEARED SUCCESSFULLY"
                                       : "CLEAR CARD FAILED",
                         3, GrayLevel::Black);
        char files[24] = {};
        char folders[24] = {};
        std::snprintf(files, sizeof(files), "%llu",
                      static_cast<unsigned long long>(
                          result.deleted_file_count));
        std::snprintf(folders, sizeof(folders), "%llu",
                      static_cast<unsigned long long>(
                          result.deleted_folder_count));
        draw_value(canvas, 196, "FILES DELETED", files);
        draw_value(canvas, 236, "FOLDERS DELETED", folders);
        draw_value(canvas, 276, "MOUNT",
                   result.mount_pass ? "PASS" : "FAIL");
        if (!result.passed) {
            canvas.draw_text(42, 326, "REASON", 2, GrayLevel::Black);
            canvas.draw_text(190, 326, result.failure_reason, 2,
                             GrayLevel::Black);
        }
    }

    canvas.draw_rect(254, 390, 292, 66, GrayLevel::Black);
    const char *label = result.attempted ? "DONE" : "CONTINUE";
    const int label_width = static_cast<int>(std::strlen(label)) * 12;
    canvas.draw_text(254 + (292 - label_width) / 2, 415,
                     label, 2, GrayLevel::Black);
}

void clear_card_confirm_page_render(Canvas &canvas)
{
    canvas.clear(GrayLevel::White);
    draw_header(canvas, "CONFIRM CLEAR CARD");
    canvas.draw_text(54, 114, "DELETE ALL FILES AND FOLDERS?", 3,
                     GrayLevel::Black);
    canvas.draw_text(54, 174,
                     "The card must remain inserted until completion.",
                     2, GrayLevel::Black);
    canvas.draw_text(54, 218,
                     "Partition and filesystem format stay unchanged.",
                     2, GrayLevel::Black);
    canvas.draw_text(54, 282, "THIS CANNOT BE UNDONE", 3,
                     GrayLevel::Black);
    canvas.draw_rect(140, 388, 230, 70, GrayLevel::Black);
    canvas.draw_text(205, 414, "CANCEL", 2, GrayLevel::Black);
    canvas.fill_rect(430, 388, 250, 70, GrayLevel::Black);
    canvas.draw_text(489, 414, "CLEAR CARD", 2, GrayLevel::White);
}

void initialize_card_page_render(Canvas &canvas, const AppState &state)
{
    const StickySdInitializeResult &result = state.initialize_card;
    canvas.clear(GrayLevel::White);
    draw_header(canvas, "INITIALIZE CARD");

    if (state.initialize_running) {
        canvas.draw_text(244, 178, "INITIALIZING...", 3,
                         GrayLevel::Black);
        canvas.draw_rect(154, 244, 492, 70, GrayLevel::Black);
        canvas.draw_text(220, 268, "DO NOT REMOVE CARD", 3,
                         GrayLevel::Black);
        return;
    }

    if (!result.attempted) {
        canvas.draw_text(54, 112,
                         "Rebuilds this card with an MBR partition.",
                         2, GrayLevel::Black);
        canvas.draw_text(54, 151,
                         "Use this when the card cannot be mounted.",
                         2, GrayLevel::Black);
        canvas.draw_rect(42, 211, 716, 74, GrayLevel::Black);
        canvas.draw_text(62, 230,
                         "Existing data on the card will be erased.",
                         2, GrayLevel::Black);
        canvas.draw_text(54, 310, "TARGET FILESYSTEM", 2,
                         GrayLevel::Black);
        canvas.draw_text(310, 310, result.target_file_system, 3,
                         GrayLevel::Black);
    } else {
        canvas.draw_rect(42, 92, 716, 58, GrayLevel::Black);
        canvas.draw_text(62, 109,
                         result.passed ? "CARD READY TO USE"
                                       : "INITIALIZATION FAILED",
                         3, GrayLevel::Black);
        draw_value(canvas, 184, "MBR",
                   result.mbr_pass ? "PASS" : "FAIL");
        draw_value(canvas, 224, result.target_file_system,
                   result.file_system_pass ? "PASS" : "FAIL");
        draw_value(canvas, 264, "MOUNT",
                   result.mount_pass ? "PASS" : "FAIL");
        if (!result.passed) {
            canvas.draw_text(42, 320, "FAILED AT", 2, GrayLevel::Black);
            canvas.draw_text(190, 320, result.failure_stage, 2,
                             GrayLevel::Black);
        }
    }

    canvas.draw_rect(254, 390, 292, 66, GrayLevel::Black);
    const char *label = result.attempted ? "DONE" : "CONTINUE";
    const int label_width = static_cast<int>(std::strlen(label)) * 12;
    canvas.draw_text(254 + (292 - label_width) / 2, 415,
                     label, 2, GrayLevel::Black);
}

void initialize_card_confirm_page_render(Canvas &canvas,
                                         const AppState &state)
{
    canvas.clear(GrayLevel::White);
    draw_header(canvas, "CONFIRM INITIALIZE");
    canvas.draw_text(54, 112, "ERASE CARD AND CREATE A NEW FILESYSTEM?", 3,
                     GrayLevel::Black);
    canvas.draw_text(54, 174,
                     "All existing partitions and data will be lost.",
                     2, GrayLevel::Black);
    canvas.draw_text(54, 218,
                     "Keep the card inserted until completion.",
                     2, GrayLevel::Black);
    canvas.draw_text(54, 258, "TARGET", 2, GrayLevel::Black);
    canvas.draw_text(190, 258, "MBR +", 2, GrayLevel::Black);
    canvas.draw_text(286, 258,
                     state.initialize_card.target_file_system, 2,
                     GrayLevel::Black);
    canvas.draw_text(54, 310, "THIS CANNOT BE UNDONE", 3,
                     GrayLevel::Black);
    canvas.draw_rect(140, 388, 230, 70, GrayLevel::Black);
    canvas.draw_text(205, 414, "CANCEL", 2, GrayLevel::Black);
    canvas.fill_rect(430, 388, 250, 70, GrayLevel::Black);
    canvas.draw_text(475, 414, "INITIALIZE", 2, GrayLevel::White);
}

void storage_test_page_render(Canvas &canvas, const AppState &state)
{
    const StickySdStorageTestResult &test = state.storage_test;
    canvas.clear(GrayLevel::White);
    draw_header(canvas, "STORAGE TEST");

    if (!test.attempted) {
        char test_size[24] = {};
        char description[64] = {};
        format_capacity(32U * 1024U, state.capacity_unit_mode,
                        test_size, sizeof(test_size));
        std::snprintf(description, sizeof(description),
                      "Writes %s to a new temporary file,", test_size);
        canvas.draw_text(54, 112, description, 2, GrayLevel::Black);
        canvas.draw_text(54, 146,
                         "reads it back, verifies CRC32 and deletes it.",
                         2, GrayLevel::Black);
        canvas.draw_text(54, 202,
                         "Existing files will not be modified.",
                         2, GrayLevel::Black);
        canvas.draw_text(54, 236,
                         "Do not remove the SD card during the test.",
                         2, GrayLevel::Black);
    } else {
        canvas.draw_rect(42, 91, 716, 58, GrayLevel::Black);
        canvas.draw_text(62, 109,
                         test.passed ? "STORAGE TEST PASSED"
                                     : "STORAGE TEST FAILED",
                         3, GrayLevel::Black);
        auto result_text = [](bool pass, bool attempted) {
            return !attempted ? "---" : (pass ? "PASS" : "FAIL");
        };
        draw_value(canvas, 174, "MOUNT",
                   result_text(test.mount_pass, true));
        draw_value(canvas, 207, "WRITE",
                   result_text(test.write_pass, test.mount_pass));
        draw_value(canvas, 240, "READ",
                   result_text(test.read_pass, test.write_pass));
        draw_value(canvas, 273, "VERIFY",
                   result_text(test.verify_pass, test.read_pass));
        draw_value(canvas, 306, "CLEANUP",
                   result_text(test.cleanup_pass,
                               test.cleanup_attempted));
        if (!test.passed) {
            canvas.draw_text(42, 350, "REASON", 2, GrayLevel::Black);
            canvas.draw_text(190, 350, test.failure_reason, 2,
                             GrayLevel::Black);
        }
    }

    canvas.draw_rect(254, 390, 292, 66, GrayLevel::Black);
    const char *label = test.attempted ? "RUN AGAIN" : "START TEST";
    const int label_width = static_cast<int>(std::strlen(label)) * 12;
    canvas.draw_text(254 + (292 - label_width) / 2, 415,
                     label, 2, GrayLevel::Black);
}

void diagnostics_page_render(Canvas &canvas, const AppState &state)
{
    const StickySdRawDiagnostics &data = state.diagnostics;
    canvas.clear(GrayLevel::White);
    draw_header(canvas, state.diagnostics_page == 0U
                            ? "SUMMARY" : "DETAILS");

    if (state.diagnostics_page == 0U) {
        char capacity[24] = {};
        format_capacity(data.capacity_bytes, state.capacity_unit_mode,
                        capacity, sizeof(capacity));
        canvas.draw_rect(42, 94, 716, 58, GrayLevel::Black);
        canvas.draw_text(64, 112, diagnostic_status(data), 3,
                         GrayLevel::Black);
        draw_value(canvas, 174, "CAPACITY",
                   data.card_initialized ? capacity : "--");
        draw_value(canvas, 207, "FILESYSTEM",
                   data.detected_file_system);
        draw_value(canvas, 240, "PARTITION", scheme_name(data.scheme));
        draw_value(canvas, 273, "MOUNT",
                   !data.mount_checked ? "NOT CHECKED" :
                   (data.mount_ok ? "OK" : "FAILED"));
        canvas.draw_text(42, 319, "MEANING", 2, GrayLevel::Black);
        canvas.draw_text(190, 319, diagnostic_problem(data), 2,
                         GrayLevel::Black);
        canvas.draw_text(42, 359, "NEXT STEP", 2, GrayLevel::Black);
        canvas.draw_text(190, 359, diagnostic_action(data), 2,
                         GrayLevel::Black);
    } else if (state.diagnostics_page == 1U) {
        char sector_size[16] = "--";
        char sector_count[24] = "--";
        char count[16] = {};
        std::snprintf(sector_size, sizeof(sector_size), "%lu B",
                      static_cast<unsigned long>(data.sector_size));
        std::snprintf(sector_count, sizeof(sector_count), "%llu",
                      static_cast<unsigned long long>(data.sector_count));
        std::snprintf(count, sizeof(count), "%u",
                      static_cast<unsigned>(
                          data.total_detected_partition_count));
        draw_value(canvas, 90, "CARD TYPE", data.card_type);
        draw_value(canvas, 119, "SECTOR SIZE", sector_size);
        draw_value(canvas, 148, "SECTOR COUNT", sector_count);
        draw_value(canvas, 177, "MBR SIG",
                   valid_text(data.mbr_signature_valid));
        if (data.gpt_checked) {
            char usable[48] = {};
            char entry_table[64] = {};
            std::snprintf(usable, sizeof(usable), "%llu - %llu",
                          static_cast<unsigned long long>(
                              data.gpt_first_usable_lba),
                          static_cast<unsigned long long>(
                              data.gpt_last_usable_lba));
            std::snprintf(entry_table, sizeof(entry_table),
                          "LBA %llu / %lu x %lu B",
                          static_cast<unsigned long long>(
                              data.gpt_partition_entry_lba),
                          static_cast<unsigned long>(
                              data.gpt_partition_entry_count),
                          static_cast<unsigned long>(
                              data.gpt_partition_entry_size));
            draw_value(canvas, 206, "PROTECTIVE MBR",
                       data.protective_mbr ? "YES" : "NO");
            draw_value(canvas, 235, "GPT HEADER CRC",
                       valid_text(data.gpt_header_crc_valid));
            draw_value(canvas, 264, "GPT ENTRY CRC",
                       data.gpt_entries_crc_checked
                           ? valid_text(data.gpt_entries_crc_valid)
                           : "NOT CHECKED");
            draw_value(canvas, 293, "USABLE LBA", usable);
            draw_value(canvas, 322, "ENTRY TABLE", entry_table);
            draw_value(canvas, 351, "PARTITIONS", count);
        } else {
            draw_value(canvas, 206, "PARTITIONS", count);
            if (data.detected_partition_count == 1U &&
                !diagnostics_has_partition_pages(data)) {
                draw_single_partition_details(
                    canvas, data.detected_partitions[0],
                    state.capacity_unit_mode);
            } else if (data.detected_partition_count == 0U) {
                canvas.draw_text(42, 252,
                                 "NO PARTITION RECORDS AVAILABLE",
                                 2, GrayLevel::Black);
            } else {
                canvas.draw_text(42, 252,
                                 "PARTITION RECORDS CONTINUE ON NEXT PAGE",
                                 2, GrayLevel::Black);
            }
        }
    } else {
        constexpr size_t kPartitionsPerPage = 3;
        const size_t first =
            (state.diagnostics_page - 2U) * kPartitionsPerPage;
        for (size_t row = 0; row < kPartitionsPerPage; ++row) {
            const size_t index = first + row;
            if (index >= data.detected_partition_count) break;
            const StickySdDetectedPartition &partition =
                data.detected_partitions[index];
            const int y = 91 + static_cast<int>(row) * 105;
            draw_partition_record(canvas, y, partition,
                                  state.capacity_unit_mode);
        }
    }

    const size_t page_count = diagnostics_page_count(state);
    char page_label[24] = {};
    std::snprintf(page_label, sizeof(page_label), "%u / %u",
                  static_cast<unsigned>(state.diagnostics_page + 1U),
                  static_cast<unsigned>(page_count));
    draw_page_arrow(canvas, 96, true);
    canvas.draw_text(356, 432, page_label, 2, GrayLevel::Black);
    draw_page_arrow(canvas, 704, false);
}
