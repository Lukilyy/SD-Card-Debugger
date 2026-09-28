#include "sticky_sdcard.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "sticky_sdcard_internal.h"

// Format an already-collected snapshot; this translation unit never touches
// the SD device or shared SPI2 bus.
namespace {

void format_report_capacity(uint64_t bytes, char *output, size_t output_size)
{
    if (output == nullptr || output_size == 0U) {
        return;
    }
    if (bytes < 1000U) {
        std::snprintf(output, output_size, "%llu bytes",
                      static_cast<unsigned long long>(bytes));
        output[output_size - 1U] = '\0';
        return;
    }

    const uint64_t decimal_unit = bytes >= 1000000000ULL
                                      ? 1000000000ULL
                                      : (bytes >= 1000000ULL ? 1000000ULL
                                                              : 1000ULL);
    const char *decimal_label = bytes >= 1000000000ULL
                                    ? "GB"
                                    : (bytes >= 1000000ULL ? "MB" : "KB");
    const uint64_t binary_unit = bytes >= 1073741824ULL
                                     ? 1073741824ULL
                                     : (bytes >= 1048576ULL ? 1048576ULL
                                                            : 1024ULL);
    const char *binary_label = bytes >= 1073741824ULL
                                   ? "GiB"
                                   : (bytes >= 1048576ULL ? "MiB" : "KiB");
    std::snprintf(output, output_size,
                  "%llu bytes / %.2f %s / %.2f %s",
                  static_cast<unsigned long long>(bytes),
                  static_cast<double>(bytes) / decimal_unit, decimal_label,
                  static_cast<double>(bytes) / binary_unit, binary_label);
    output[output_size - 1U] = '\0';
}

}  // namespace

void sticky_sdcard_log_report(const StickySdRawDiagnostics &diagnostics,
                              const StickySdCardInfo &mount_info)
{
    constexpr char kReportTag[] = "sd_card_report";
    ESP_LOGI(kReportTag, "================ SD CARD REPORT ================");
    if (!diagnostics.inserted) {
        ESP_LOGI(kReportTag, "PRESENCE        NOT INSERTED");
        ESP_LOGI(kReportTag, "CARD STATUS     NO CARD");
        ESP_LOGI(kReportTag, "================================================");
        return;
    }

    ESP_LOGI(kReportTag, "PRESENCE        INSERTED");
    ESP_LOGI(kReportTag, "RAW INIT        %s",
             diagnostics.card_initialized ? "PASS" : "FAIL");
    ESP_LOGI(kReportTag, "RAW READ        %s",
             diagnostics.sector0_read ? "PASS" : "FAIL");

    if (diagnostics.card_initialized) {
        const StickySdCardRegisters &card = diagnostics.card;
        char capacity[80] = {};
        format_report_capacity(diagnostics.capacity_bytes, capacity,
                               sizeof(capacity));
        ESP_LOGI(kReportTag, "[INTERFACE]");
        ESP_LOGI(kReportTag, "HOST            SDSPI / SPI2");
        if (card.actual_clock_khz > 0) {
            ESP_LOGI(kReportTag, "SPI CLOCK       %.2f MHz actual",
                     static_cast<double>(card.actual_clock_khz) / 1000.0);
        } else {
            ESP_LOGI(kReportTag, "SPI CLOCK       unavailable");
        }
        ESP_LOGI(kReportTag, "HOST LIMIT      %.2f MHz",
                 static_cast<double>(card.host_limit_khz) / 1000.0);
        if (card.advertised_transfer_hz != 0U) {
            ESP_LOGI(kReportTag, "CSD TRANSFER    %.2f MHz advertised",
                     static_cast<double>(card.advertised_transfer_hz) /
                         1000000.0);
        } else {
            ESP_LOGI(kReportTag, "CSD TRANSFER    unavailable");
        }

        ESP_LOGI(kReportTag, "[CARD]");
        ESP_LOGI(kReportTag, "TYPE            %s", diagnostics.card_type);
        ESP_LOGI(kReportTag, "CAPACITY        %s", capacity);
        ESP_LOGI(kReportTag, "SECTOR SIZE     %lu B",
                 static_cast<unsigned long>(diagnostics.sector_size));
        ESP_LOGI(kReportTag, "SECTOR COUNT    %llu",
                 static_cast<unsigned long long>(diagnostics.sector_count));
        ESP_LOGI(kReportTag, "OCR             0x%08lX",
                 static_cast<unsigned long>(card.ocr));

        const unsigned revision_major = card.product_revision >> 4U;
        const unsigned revision_minor = card.product_revision & 0x0FU;
        const unsigned manufacture_year =
            2000U + (card.manufacturing_date >> 4U);
        const unsigned manufacture_month = card.manufacturing_date & 0x0FU;
        const char oem_first = static_cast<char>(card.oem_id >> 8U);
        const char oem_second = static_cast<char>(card.oem_id & 0xFFU);
        const bool printable_oem = oem_first >= 0x20 && oem_first <= 0x7E &&
                                   oem_second >= 0x20 && oem_second <= 0x7E;
        ESP_LOGI(kReportTag, "[CID]");
        ESP_LOGI(kReportTag, "MID             0x%02X",
                 static_cast<unsigned>(card.manufacturer_id));
        if (printable_oem) {
            ESP_LOGI(kReportTag, "OID             0x%04X ('%c%c')",
                     static_cast<unsigned>(card.oem_id), oem_first,
                     oem_second);
        } else {
            ESP_LOGI(kReportTag, "OID             0x%04X",
                     static_cast<unsigned>(card.oem_id));
        }
        ESP_LOGI(kReportTag, "PRODUCT         %s", card.product_name);
        ESP_LOGI(kReportTag, "REVISION        %u.%u", revision_major,
                 revision_minor);
        ESP_LOGI(kReportTag, "SERIAL          0x%08lX",
                 static_cast<unsigned long>(card.product_serial));
        if (manufacture_month >= 1U && manufacture_month <= 12U) {
            ESP_LOGI(kReportTag, "MFG DATE        %04u-%02u",
                     manufacture_year, manufacture_month);
        } else {
            ESP_LOGI(kReportTag, "MFG DATE        unavailable (raw=0x%03X)",
                     static_cast<unsigned>(card.manufacturing_date));
        }

        ESP_LOGI(kReportTag, "[CSD / SCR / SSR]");
        ESP_LOGI(kReportTag, "CSD VERSION     %ld",
                 static_cast<long>(card.csd_version + 1));
        ESP_LOGI(kReportTag, "READ BLOCK      %lu B",
                 static_cast<unsigned long>(card.read_block_size));
        ESP_LOGI(kReportTag, "COMMAND CLASS   0x%03lX",
                 static_cast<unsigned long>(card.command_classes));
        ESP_LOGI(kReportTag, "SD SPEC         %u (decoded code)",
                 static_cast<unsigned>(card.sd_spec));
        ESP_LOGI(kReportTag, "BUS SUPPORT     %s%s",
                 (card.supported_bus_widths & 0x01U) != 0U ? "1-bit" : "--",
                 (card.supported_bus_widths & 0x04U) != 0U ? ", 4-bit" : "");
        ESP_LOGI(kReportTag, "ACTIVE LINK     SPI (SSR width=%u-bit)",
                 static_cast<unsigned>(card.current_bus_width));
        ESP_LOGI(kReportTag, "CARD AU         %lu KiB",
                 static_cast<unsigned long>(card.allocation_unit_kib));
        ESP_LOGI(kReportTag, "ERASE SIZE      %lu allocation units",
                 static_cast<unsigned long>(card.erase_size_au));
        ESP_LOGI(kReportTag, "DISCARD         %s",
                 card.discard_supported ? "SUPPORTED" : "NOT REPORTED");
        ESP_LOGI(kReportTag, "FULL AREA ERASE %s",
                 card.full_user_area_erase_supported ? "SUPPORTED"
                                                     : "NOT REPORTED");
    }

    ESP_LOGI(kReportTag, "[LAYOUT]");
    ESP_LOGI(kReportTag, "SCHEME          %s",
             sdcard_internal::partition_scheme_name(diagnostics.scheme));
    ESP_LOGI(kReportTag, "MBR SIGNATURE   %s",
             diagnostics.mbr_signature_valid ? "VALID" : "INVALID");
    ESP_LOGI(kReportTag, "PROTECTIVE MBR  %s",
             diagnostics.protective_mbr ? "YES" : "NO");
    if (diagnostics.gpt_checked) {
        ESP_LOGI(kReportTag, "GPT HEADER      %s (CRC %s)",
                 diagnostics.gpt_valid ? "VALID" : "INVALID",
                 diagnostics.gpt_header_crc_valid ? "VALID" : "INVALID");
        ESP_LOGI(kReportTag, "GPT ENTRIES CRC %s",
                 diagnostics.gpt_entries_crc_checked
                     ? (diagnostics.gpt_entries_crc_valid ? "VALID" : "INVALID")
                     : "NOT CHECKED");
        ESP_LOGI(kReportTag, "GPT USABLE LBA  %llu..%llu",
                 static_cast<unsigned long long>(
                     diagnostics.gpt_first_usable_lba),
                 static_cast<unsigned long long>(
                     diagnostics.gpt_last_usable_lba));
    }
    size_t valid_partition_count = 0;
    for (size_t index = 0; index < diagnostics.detected_partition_count;
         ++index) {
        if (diagnostics.detected_partitions[index].range_valid) {
            ++valid_partition_count;
        }
    }
    ESP_LOGI(kReportTag, "PARTITIONS      %u valid",
             static_cast<unsigned>(valid_partition_count));
    for (size_t index = 0; index < diagnostics.detected_partition_count;
         ++index) {
        const StickySdDetectedPartition &partition =
            diagnostics.detected_partitions[index];
        if (!partition.range_valid) {
            continue;
        }
        char partition_size[80] = {};
        const uint64_t bytes = partition.sector_count *
                               diagnostics.sector_size;
        format_report_capacity(bytes, partition_size, sizeof(partition_size));
        ESP_LOGI(kReportTag, "[PARTITION %lu]",
                 static_cast<unsigned long>(partition.table_index));
        ESP_LOGI(kReportTag, "SOURCE          %s",
                 partition.from_gpt ? "GPT" : "MBR/RAW");
        if (partition.from_gpt) {
            ESP_LOGI(kReportTag, "TYPE            %s",
                     partition.partition_type);
        } else {
            ESP_LOGI(kReportTag, "TYPE            0x%02X (%s)",
                     static_cast<unsigned>(partition.mbr_type),
                     partition.partition_type);
        }
        ESP_LOGI(kReportTag, "START LBA       %llu",
                 static_cast<unsigned long long>(partition.start_lba));
        ESP_LOGI(kReportTag, "SECTOR COUNT    %llu",
                 static_cast<unsigned long long>(partition.sector_count));
        ESP_LOGI(kReportTag, "SIZE            %s", partition_size);
        ESP_LOGI(kReportTag, "RANGE           %s",
                 partition.range_valid ? "VALID" : "INVALID");
        ESP_LOGI(kReportTag, "BOOT SECTOR     %s",
                 partition.boot_sector_read ? "READ PASS" : "NOT READ");
        ESP_LOGI(kReportTag, "FILESYSTEM      %s",
                 partition.file_system);
        if (partition.cluster_size != 0U) {
            ESP_LOGI(kReportTag, "CLUSTER         %lu bytes",
                     static_cast<unsigned long>(partition.cluster_size));
        } else {
            ESP_LOGI(kReportTag, "CLUSTER         unavailable");
        }
    }

    ESP_LOGI(kReportTag, "[VOLUME]");
    if (diagnostics.mount_checked) {
        ESP_LOGI(kReportTag, "MOUNT           %s (%s)",
                 diagnostics.mount_ok ? "PASS" : "FAIL",
                 esp_err_to_name(diagnostics.mount_error));
    } else {
        ESP_LOGI(kReportTag, "MOUNT           NOT ATTEMPTED");
    }
    if (diagnostics.mount_ok) {
        char total[80] = {};
        char used[80] = {};
        char free[80] = {};
        format_report_capacity(mount_info.total_bytes, total, sizeof(total));
        format_report_capacity(mount_info.used_bytes, used, sizeof(used));
        format_report_capacity(mount_info.free_bytes, free, sizeof(free));
        ESP_LOGI(kReportTag, "FILESYSTEM      %s", mount_info.file_system);
        ESP_LOGI(kReportTag, "TOTAL           %s", total);
        ESP_LOGI(kReportTag, "USED            %s", used);
        ESP_LOGI(kReportTag, "FREE            %s", free);
        ESP_LOGI(kReportTag, "CLUSTER         %lu bytes",
                 static_cast<unsigned long>(mount_info.cluster_size));
    } else {
        ESP_LOGI(kReportTag, "FILESYSTEM      %s (raw detection)",
                 diagnostics.detected_file_system);
        ESP_LOGI(kReportTag, "VOLUME USAGE    unavailable");
    }

    const char *status = "RAW ACCESS FAILED";
    if (!diagnostics.card_initialized) {
        status = "CARD INITIALIZATION FAILED";
    } else if (!diagnostics.sector0_read) {
        status = "RAW READ FAILED";
    } else if (diagnostics.mount_ok) {
        status = "READY";
    } else if (std::strcmp(diagnostics.detected_file_system, "UNKNOWN") != 0) {
        status = "FILESYSTEM DETECTED, MOUNT FAILED";
    } else {
        status = "CARD READABLE, FILESYSTEM UNAVAILABLE";
    }
    ESP_LOGI(kReportTag, "[RESULT]");
    ESP_LOGI(kReportTag, "CARD STATUS     %s", status);
    ESP_LOGI(kReportTag, "================================================");
}
