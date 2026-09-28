#pragma once

#include <cstdint>
#include <cstddef>

#include "esp_err.h"

struct StickySdCardInfo {
    bool inserted = false;
    bool mounted = false;
    bool raw_readable = false;
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t sector_count = 0;
    uint32_t sector_size = 0;
    uint32_t cluster_size = 0;
    char file_system[12] = "UNKNOWN";
    char partition[16] = "UNKNOWN";
    char card_type[12] = "UNKNOWN";
    esp_err_t error = ESP_ERR_INVALID_STATE;
};

constexpr size_t kStickySdFileNameSize = 256;

struct StickySdDirectoryEntry {
    char name[kStickySdFileNameSize] = {};
    uint64_t size = 0;
    int64_t modified_time = 0;
    bool is_directory = false;
};

struct StickySdDirectoryStats {
    uint64_t total_file_bytes = 0;
    uint64_t file_count = 0;
    uint64_t folder_count = 0;
};

enum class StickySdPartitionScheme : uint8_t {
    Unknown,
    Superfloppy,
    Mbr,
    Gpt,
};

struct StickySdMbrPartition {
    uint8_t status = 0;
    uint8_t type = 0;
    uint32_t start_lba = 0;
    uint32_t sector_count = 0;
    bool present = false;
    bool range_valid = false;
};

constexpr size_t kStickySdDiagnosticPartitionCount = 8;

struct StickySdDetectedPartition {
    uint32_t table_index = 0;
    uint8_t mbr_type = 0;
    uint64_t start_lba = 0;
    uint64_t sector_count = 0;
    bool from_gpt = false;
    bool range_valid = false;
    bool boot_sector_read = false;
    uint32_t cluster_size = 0;
    uint64_t exfat_partition_offset = 0;
    uint64_t exfat_volume_length = 0;
    char partition_type[20] = "UNKNOWN";
    char file_system[12] = "UNKNOWN";
};

// Card-register and active transport data captured while the temporary raw
// SDSPI session is alive. Only decoded scalar values are retained; no host or
// device handle escapes the shared-SPI2 acquire/release boundary.
struct StickySdCardRegisters {
    uint32_t ocr = 0;
    uint32_t host_limit_khz = 0;
    int32_t actual_clock_khz = 0;
    uint8_t manufacturer_id = 0;
    uint16_t oem_id = 0;
    char product_name[8] = {};
    uint8_t product_revision = 0;
    uint32_t product_serial = 0;
    uint16_t manufacturing_date = 0;
    int32_t csd_version = 0;
    uint32_t read_block_size = 0;
    uint32_t command_classes = 0;
    uint32_t advertised_transfer_hz = 0;
    uint8_t sd_spec = 0;
    uint8_t supported_bus_widths = 0;
    uint8_t current_bus_width = 0;
    uint32_t allocation_unit_kib = 0;
    uint32_t erase_size_au = 0;
    bool discard_supported = false;
    bool full_user_area_erase_supported = false;
};

struct StickySdRawDiagnostics {
    bool inserted = false;
    bool card_initialized = false;
    bool sector0_read = false;
    bool mbr_signature_valid = false;
    bool protective_mbr = false;
    bool gpt_checked = false;
    bool gpt_valid = false;
    bool gpt_header_crc_valid = false;
    bool gpt_entries_crc_checked = false;
    bool gpt_entries_crc_valid = false;
    bool mount_checked = false;
    bool mount_ok = false;
    uint64_t sector_count = 0;
    uint64_t capacity_bytes = 0;
    uint32_t sector_size = 0;
    char card_type[12] = "UNKNOWN";
    StickySdCardRegisters card = {};
    StickySdPartitionScheme scheme = StickySdPartitionScheme::Unknown;
    StickySdMbrPartition partitions[4] = {};
    uint64_t gpt_first_usable_lba = 0;
    uint64_t gpt_last_usable_lba = 0;
    uint64_t gpt_partition_entry_lba = 0;
    uint32_t gpt_partition_entry_count = 0;
    uint32_t gpt_partition_entry_size = 0;
    uint32_t gpt_partition_entry_crc = 0;
    StickySdDetectedPartition
        detected_partitions[kStickySdDiagnosticPartitionCount] = {};
    size_t detected_partition_count = 0;
    size_t total_detected_partition_count = 0;
    char detected_file_system[12] = "UNKNOWN";
    esp_err_t mount_error = ESP_ERR_INVALID_STATE;
    esp_err_t error = ESP_ERR_INVALID_STATE;
};

struct StickySdStorageTestResult {
    bool attempted = false;
    bool mount_pass = false;
    bool create_pass = false;
    bool write_pass = false;
    bool read_pass = false;
    bool verify_pass = false;
    bool cleanup_attempted = false;
    bool cleanup_pass = false;
    bool passed = false;
    uint32_t bytes_tested = 0;
    uint32_t expected_crc32 = 0;
    uint32_t actual_crc32 = 0;
    char failure_reason[48] = "NOT STARTED";
    esp_err_t error = ESP_ERR_INVALID_STATE;
};

struct StickySdClearCardResult {
    bool attempted = false;
    bool mount_pass = false;
    bool passed = false;
    uint64_t deleted_file_count = 0;
    uint64_t deleted_folder_count = 0;
    char failure_reason[48] = "NOT STARTED";
    esp_err_t error = ESP_ERR_INVALID_STATE;
};

struct StickySdInitializeResult {
    bool attempted = false;
    bool target_known = false;
    bool target_exfat = false;
    bool format_pass = false;
    bool mbr_pass = false;
    bool file_system_pass = false;
    bool mount_pass = false;
    bool passed = false;
    uint64_t capacity_bytes = 0;
    char target_file_system[8] = "UNKNOWN";
    char failure_stage[48] = "NOT STARTED";
    esp_err_t error = ESP_ERR_INVALID_STATE;
};

// Configures card detection, power, and chip-select GPIOs. SPI2 must already
// be initialized by the display/shared bus owner.
esp_err_t sticky_sdcard_init();

// Reads the active-low card-detect GPIO. Does not mount or access SPI2.
bool sticky_sdcard_is_inserted();

// Mounts the card on the existing shared SPI2 bus, reads card/filesystem
// metadata, then unmounts it before the display uses the shared bus again.
// No file or filesystem content is modified.
esp_err_t sticky_sdcard_get_info(StickySdCardInfo &info);

// Initializes the card directly through SDSPI and reads only raw sectors.
// This does not mount a filesystem and never writes, formats, erases, or
// repairs card contents. The temporary SDSPI device is removed before return.
esp_err_t sticky_sdcard_get_raw_diagnostics(
    StickySdRawDiagnostics &diagnostics);

// Formats an already-collected raw + mount snapshot. This function performs
// no SD, SPI, filesystem, or GPIO access.
void sticky_sdcard_log_report(const StickySdRawDiagnostics &diagnostics,
                              const StickySdCardInfo &mount_info);

// Runs a bounded write/read/verify test using a new exclusive temporary file.
// Existing files are never opened for writing. The temporary file is removed
// on every path where it was successfully created.
esp_err_t sticky_sdcard_run_storage_test(StickySdStorageTestResult &test);

// Deletes every child below the mounted SD root while preserving the root,
// partition table, and filesystem. No formatting or raw-sector writes occur.
esp_err_t sticky_sdcard_clear_contents(StickySdClearCardResult &result);

// Reads raw capacity and selects the filesystem which Initialize Card will
// use. No filesystem mount or write is performed.
esp_err_t sticky_sdcard_probe_initialize_target(
    StickySdInitializeResult &result);

// Replaces the card layout with one MBR FAT32 or exFAT partition selected by
// capacity, then independently verifies the raw layout and normal VFS mount.
esp_err_t sticky_sdcard_initialize_card(StickySdInitializeResult &result);

// Mounts the card, reads one directory, and unmounts it before returning.
// directory_path is an absolute path relative to the SD root (for example
// "/" or "/photos"). Entries before entry_offset are skipped and
// total_entry_count reports the number of readable entries in the directory.
esp_err_t sticky_sdcard_list_directory(const char *directory_path,
                                       StickySdDirectoryEntry *entries,
                                       size_t entry_capacity,
                                       size_t entry_offset,
                                       size_t &entry_count,
                                       size_t &total_entry_count);

// Recursively measures the contents below one root-relative directory.
// The target directory itself is not included in folder_count.
esp_err_t sticky_sdcard_get_directory_stats(
    const char *directory_path,
    StickySdDirectoryStats &stats);

// Deletes exactly one regular file from an absolute directory below the SD
// root. Directories and names containing path separators are rejected.
esp_err_t sticky_sdcard_delete_file(const char *directory_path,
                                    const char *file_name);

// Recursively deletes exactly one directory below the SD root. The parent
// path and folder name are validated as root-relative components; the SD root
// itself can never be selected.
esp_err_t sticky_sdcard_delete_directory(const char *directory_path,
                                         const char *directory_name);

// Creates one empty regular file or directory without replacing an existing
// entry. The name must be a single path component.
esp_err_t sticky_sdcard_create_entry(const char *directory_path,
                                     const char *entry_name,
                                     bool create_directory);
