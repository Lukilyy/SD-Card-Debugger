#include "sticky_sdcard.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_config.h"
#include "sticky_rtc.h"
#include "sticky_sdcard_internal.h"
#include "sd_protocol_defs.h"
#include "sdmmc_cmd.h"

namespace {

constexpr char kTag[] = "sticky_sdcard";
constexpr char kMountPoint[] = "/sdcard";
constexpr TickType_t kPowerOnDelay = pdMS_TO_TICKS(100);
constexpr size_t kMaximumSdPathSize = 640;
constexpr size_t kMaximumDeleteDepth = 32;
constexpr uint64_t kFat32MaximumCardBytes = 32000000000ULL;
constexpr uint64_t kInitializeMaximumCardBytes = 256000000000ULL;
constexpr uint32_t kInitializePartitionStartLba = 2048U;
constexpr BYTE kInitializeFatFsDrive = 1U;

const char *sd_card_capacity_class(const sdmmc_card_t &card)
{
    if ((card.ocr & SD_OCR_SDHC_CAP) == 0U) {
        return "SDSC";
    }
    const uint64_t capacity_bytes =
        static_cast<uint64_t>(card.csd.capacity) * card.csd.sector_size;
    return capacity_bytes > kFat32MaximumCardBytes ? "SDXC" : "SDHC";
}

bool select_initialize_file_system(uint64_t capacity_bytes,
                                   StickySdInitializeResult &result)
{
    result.capacity_bytes = capacity_bytes;
    result.target_known = capacity_bytes != 0U &&
        capacity_bytes <= kInitializeMaximumCardBytes;
    if (!result.target_known) {
        std::snprintf(result.target_file_system,
                      sizeof(result.target_file_system), "UNKNOWN");
        return false;
    }
    result.target_exfat = capacity_bytes > kFat32MaximumCardBytes;
    std::snprintf(result.target_file_system,
                  sizeof(result.target_file_system), "%s",
                  result.target_exfat ? "exFAT" : "FAT32");
    return true;
}

bool valid_fat_time(time_t value)
{
    struct tm local_time = {};
    return localtime_r(&value, &local_time) != nullptr &&
           local_time.tm_year >= 80 && local_time.tm_year <= 207;
}

void apply_and_verify_entry_time(const char *path, time_t value)
{
    if (path == nullptr || !valid_fat_time(value)) {
        ESP_LOGW(kTag, "FAT timestamp unavailable for %s",
                 path != nullptr ? path : "(null)");
        return;
    }

    const struct utimbuf timestamp = {
        .actime = value,
        .modtime = value,
    };
    if (utime(path, &timestamp) != 0) {
        ESP_LOGE(kTag, "Timestamp update failed for %s: errno=%d (%s)",
                 path, errno, std::strerror(errno));
        return;
    }

    struct stat status = {};
    if (stat(path, &status) != 0) {
        ESP_LOGE(kTag, "Timestamp readback failed for %s: errno=%d (%s)",
                 path, errno, std::strerror(errno));
        return;
    }

    struct tm written_time = {};
    if (localtime_r(&status.st_mtime, &written_time) == nullptr) {
        ESP_LOGE(kTag, "Timestamp conversion failed for %s", path);
        return;
    }
    const double difference = std::difftime(status.st_mtime, value);
    if (difference < -1.0 || difference > 1.0) {
        ESP_LOGE(kTag,
                 "FAT timestamp readback does not match the requested RTC time for %s",
                 path);
        return;
    }
    ESP_LOGI(kTag,
             "FAT timestamp verified for %s: %04d-%02d-%02d %02d:%02d:%02d",
             path,
             written_time.tm_year + 1900,
             written_time.tm_mon + 1,
             written_time.tm_mday,
             written_time.tm_hour,
             written_time.tm_min,
             written_time.tm_sec);
}
constexpr size_t kMbrPartitionTableOffset = 446;
constexpr size_t kMbrPartitionEntrySize = 16;

bool s_initialized = false;

bool valid_entry_name(const char *name)
{
    return name != nullptr && name[0] != '\0' &&
           std::strcmp(name, ".") != 0 &&
           std::strcmp(name, "..") != 0 &&
           std::strchr(name, '/') == nullptr &&
           std::strchr(name, '\\') == nullptr;
}

bool card_is_inserted();

bool valid_directory_path(const char *path)
{
    if (path == nullptr || path[0] != '/') {
        return false;
    }
    if (path[1] == '\0') {
        return true;
    }

    const char *component = path + 1;
    while (*component != '\0') {
        const char *separator = std::strchr(component, '/');
        const size_t length = separator != nullptr
                                  ? static_cast<size_t>(separator - component)
                                  : std::strlen(component);
        if (length == 0U ||
            (length == 1U && component[0] == '.') ||
            (length == 2U && component[0] == '.' && component[1] == '.') ||
            std::memchr(component, '\\', length) != nullptr) {
            return false;
        }
        if (separator == nullptr) {
            return true;
        }
        component = separator + 1;
    }
    return false;
}

esp_err_t filesystem_error(int error)
{
    if (error == ENOENT) {
        return ESP_ERR_NOT_FOUND;
    }
    if (error == ENOMEM) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_FAIL;
}

esp_err_t scan_directory_tree(const char *directory_path,
                              size_t depth,
                              StickySdDirectoryStats &stats)
{
    if (depth > kMaximumDeleteDepth) {
        ESP_LOGE(kTag, "Directory nesting is too deep: %s", directory_path);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!card_is_inserted()) {
        return ESP_ERR_NOT_FOUND;
    }

    struct stat directory_status = {};
    if (stat(directory_path, &directory_status) != 0) {
        ESP_LOGE(kTag, "Directory stat failed for %s: errno=%d (%s)",
                 directory_path, errno, std::strerror(errno));
        return filesystem_error(errno);
    }
    if (!S_ISDIR(directory_status.st_mode)) {
        return ESP_ERR_INVALID_ARG;
    }

    DIR *directory = opendir(directory_path);
    if (directory == nullptr) {
        ESP_LOGE(kTag, "Open directory failed for %s: errno=%d (%s)",
                 directory_path, errno, std::strerror(errno));
        return filesystem_error(errno);
    }

    esp_err_t result = ESP_OK;
    while (result == ESP_OK) {
        if (!card_is_inserted()) {
            result = ESP_ERR_NOT_FOUND;
            break;
        }
        errno = 0;
        dirent *entry = readdir(directory);
        if (entry == nullptr) {
            if (errno != 0) {
                result = filesystem_error(errno);
            }
            break;
        }
        if (std::strcmp(entry->d_name, ".") == 0 ||
            std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!valid_entry_name(entry->d_name)) {
            result = ESP_ERR_INVALID_ARG;
            break;
        }

        const size_t parent_length = std::strlen(directory_path);
        const size_t name_length = std::strlen(entry->d_name);
        const size_t child_size = parent_length + 1U + name_length + 1U;
        if (child_size > kMaximumSdPathSize) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        char *child_path = static_cast<char *>(std::malloc(child_size));
        if (child_path == nullptr) {
            result = ESP_ERR_NO_MEM;
            break;
        }
        std::memcpy(child_path, directory_path, parent_length);
        child_path[parent_length] = '/';
        std::memcpy(child_path + parent_length + 1U,
                    entry->d_name, name_length + 1U);

        struct stat child_status = {};
        if (stat(child_path, &child_status) != 0) {
            ESP_LOGE(kTag, "Entry stat failed for %s: errno=%d (%s)",
                     child_path, errno, std::strerror(errno));
            result = filesystem_error(errno);
        } else if (S_ISDIR(child_status.st_mode)) {
            if (stats.folder_count ==
                std::numeric_limits<uint64_t>::max()) {
                result = ESP_ERR_INVALID_SIZE;
            } else {
                ++stats.folder_count;
                result = scan_directory_tree(child_path, depth + 1U, stats);
            }
        } else if (S_ISREG(child_status.st_mode)) {
            const uint64_t file_size =
                static_cast<uint64_t>(child_status.st_size);
            if (stats.file_count == std::numeric_limits<uint64_t>::max() ||
                file_size > std::numeric_limits<uint64_t>::max() -
                                stats.total_file_bytes) {
                result = ESP_ERR_INVALID_SIZE;
            } else {
                ++stats.file_count;
                stats.total_file_bytes += file_size;
            }
        } else {
            result = ESP_ERR_NOT_SUPPORTED;
        }
        std::free(child_path);
    }

    if (closedir(directory) != 0 && result == ESP_OK) {
        result = filesystem_error(errno);
    }
    return result;
}

esp_err_t delete_directory_tree(const char *directory_path,
                                size_t depth,
                                uint64_t *deleted_files = nullptr,
                                uint64_t *deleted_folders = nullptr)
{
    if (depth > kMaximumDeleteDepth) {
        ESP_LOGE(kTag, "Directory nesting is too deep: %s", directory_path);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!card_is_inserted()) {
        return ESP_ERR_NOT_FOUND;
    }

    struct stat directory_status = {};
    if (stat(directory_path, &directory_status) != 0) {
        ESP_LOGE(kTag, "Directory stat failed for %s: errno=%d (%s)",
                 directory_path, errno, std::strerror(errno));
        return filesystem_error(errno);
    }
    if (!S_ISDIR(directory_status.st_mode)) {
        ESP_LOGW(kTag, "Recursive delete rejected non-directory: %s",
                 directory_path);
        return ESP_ERR_INVALID_ARG;
    }

    while (true) {
        if (!card_is_inserted()) {
            return ESP_ERR_NOT_FOUND;
        }

        DIR *directory = opendir(directory_path);
        if (directory == nullptr) {
            ESP_LOGE(kTag, "Open directory failed for %s: errno=%d (%s)",
                     directory_path, errno, std::strerror(errno));
            return filesystem_error(errno);
        }

        char *child_path = nullptr;
        esp_err_t scan_result = ESP_OK;
        while (child_path == nullptr) {
            errno = 0;
            dirent *entry = readdir(directory);
            if (entry == nullptr) {
                if (errno != 0) {
                    scan_result = filesystem_error(errno);
                }
                break;
            }
            if (std::strcmp(entry->d_name, ".") == 0 ||
                std::strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            if (!valid_entry_name(entry->d_name)) {
                ESP_LOGE(kTag, "Unsafe directory entry rejected in %s: %s",
                         directory_path, entry->d_name);
                scan_result = ESP_ERR_INVALID_ARG;
                break;
            }

            const size_t parent_length = std::strlen(directory_path);
            const size_t name_length = std::strlen(entry->d_name);
            const size_t child_size = parent_length + 1U + name_length + 1U;
            if (child_size > kMaximumSdPathSize) {
                scan_result = ESP_ERR_INVALID_SIZE;
                break;
            }
            child_path = static_cast<char *>(std::malloc(child_size));
            if (child_path == nullptr) {
                scan_result = ESP_ERR_NO_MEM;
                break;
            }
            std::memcpy(child_path, directory_path, parent_length);
            child_path[parent_length] = '/';
            std::memcpy(child_path + parent_length + 1U,
                        entry->d_name, name_length + 1U);
        }

        if (closedir(directory) != 0 && scan_result == ESP_OK) {
            scan_result = filesystem_error(errno);
        }
        if (scan_result != ESP_OK) {
            std::free(child_path);
            return scan_result;
        }
        if (child_path == nullptr) {
            break;
        }

        esp_err_t child_result = ESP_OK;
        struct stat child_status = {};
        if (stat(child_path, &child_status) != 0) {
            ESP_LOGE(kTag, "Entry stat failed for %s: errno=%d (%s)",
                     child_path, errno, std::strerror(errno));
            child_result = filesystem_error(errno);
        } else if (S_ISDIR(child_status.st_mode)) {
            child_result = delete_directory_tree(
                child_path, depth + 1U, deleted_files, deleted_folders);
        } else if (S_ISREG(child_status.st_mode)) {
            if (unlink(child_path) != 0) {
                ESP_LOGE(kTag, "Delete file failed for %s: errno=%d (%s)",
                         child_path, errno, std::strerror(errno));
                child_result = filesystem_error(errno);
            } else if (deleted_files != nullptr) {
                ++(*deleted_files);
            }
        } else {
            ESP_LOGE(kTag, "Special filesystem entry rejected: %s",
                     child_path);
            child_result = ESP_ERR_NOT_SUPPORTED;
        }
        std::free(child_path);
        if (child_result != ESP_OK) {
            return child_result;
        }
    }

    if (!card_is_inserted()) {
        return ESP_ERR_NOT_FOUND;
    }
    if (rmdir(directory_path) != 0) {
        ESP_LOGE(kTag, "Delete directory failed for %s: errno=%d (%s)",
                 directory_path, errno, std::strerror(errno));
        return filesystem_error(errno);
    }
    if (deleted_folders != nullptr) {
        ++(*deleted_folders);
    }
    return ESP_OK;
}

esp_err_t build_entry_path(const char *directory_path,
                           const char *entry_name,
                           char *output,
                           size_t output_size)
{
    if (directory_path == nullptr || directory_path[0] != '/' ||
        !valid_entry_name(entry_name) || output == nullptr ||
        output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *separator =
        std::strcmp(directory_path, "/") == 0 ? "" : "/";
    const int length = std::snprintf(output,
                                     output_size,
                                     "%s%s%s%s",
                                     kMountPoint,
                                     directory_path,
                                     separator,
                                     entry_name);
    return length > 0 && static_cast<size_t>(length) < output_size
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

uint16_t read_le16(const uint8_t *data)
{
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(data[1] << 8U);
}

uint32_t read_le32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8U) |
           (static_cast<uint32_t>(data[2]) << 16U) |
           (static_cast<uint32_t>(data[3]) << 24U);
}

uint64_t read_le64(const uint8_t *data)
{
    return static_cast<uint64_t>(read_le32(data)) |
           (static_cast<uint64_t>(read_le32(data + 4)) << 32U);
}

void write_le32(uint8_t *data, uint32_t value)
{
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8U);
    data[2] = static_cast<uint8_t>(value >> 16U);
    data[3] = static_cast<uint8_t>(value >> 24U);
}

void encode_compatible_mbr_chs(uint32_t lba, uint8_t *output)
{
    constexpr uint32_t kHeads = 255U;
    constexpr uint32_t kSectorsPerTrack = 63U;
    const uint32_t cylinder = lba / (kHeads * kSectorsPerTrack);
    if (cylinder > 1023U) {
        output[0] = 0xFEU;
        output[1] = 0xFFU;
        output[2] = 0xFFU;
        return;
    }

    const uint32_t head = (lba / kSectorsPerTrack) % kHeads;
    const uint32_t sector = lba % kSectorsPerTrack + 1U;
    output[0] = static_cast<uint8_t>(head);
    output[1] = static_cast<uint8_t>(
        (sector & 0x3FU) | ((cylinder >> 2U) & 0xC0U));
    output[2] = static_cast<uint8_t>(cylinder);
}

esp_err_t write_initialize_mbr(sdmmc_card_t *card,
                               uint8_t partition_type,
                               uint32_t &partition_sector_count)
{
    if (card == nullptr || card->csd.sector_size != 512U ||
        card->csd.capacity <=
            static_cast<int>(kInitializePartitionStartLba)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint64_t count = static_cast<uint64_t>(card->csd.capacity) -
                           kInitializePartitionStartLba;
    if (count == 0U || count > std::numeric_limits<uint32_t>::max()) {
        return ESP_ERR_INVALID_SIZE;
    }
    partition_sector_count = static_cast<uint32_t>(count);

    uint8_t *sector = static_cast<uint8_t *>(heap_caps_calloc(
        1U, card->csd.sector_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (sector == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // Remove stale primary/backup GPT headers before publishing the new MBR.
    // The partition begins at LBA 2048, so LBA 1 is outside the new volume;
    // the final sector is inside it and will remain ordinary zeroed excess
    // space unless the filesystem later needs it.
    esp_err_t result = sdmmc_write_sectors(card, sector, 1U, 1U);
    if (result == ESP_OK) {
        result = sdmmc_write_sectors(
            card, sector, static_cast<size_t>(card->csd.capacity - 1), 1U);
    }
    if (result != ESP_OK) {
        heap_caps_free(sector);
        return result;
    }

    uint8_t *entry = sector + kMbrPartitionTableOffset;
    entry[0] = 0x00U;
    encode_compatible_mbr_chs(kInitializePartitionStartLba, entry + 1);
    entry[4] = partition_type;
    const uint32_t last_lba = kInitializePartitionStartLba +
                              partition_sector_count - 1U;
    encode_compatible_mbr_chs(last_lba, entry + 5);
    write_le32(entry + 8, kInitializePartitionStartLba);
    write_le32(entry + 12, partition_sector_count);
    sector[510] = 0x55U;
    sector[511] = 0xAAU;

    result = sdmmc_write_sectors(card, sector, 0U, 1U);
    heap_caps_free(sector);
    return result;
}

void copy_label(char *destination, size_t size, const char *source);

uint32_t crc32_ieee_update(uint32_t crc, const uint8_t *data, size_t size)
{
    for (size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^
                  ((crc & 1U) != 0U ? 0xEDB88320U : 0U);
        }
    }
    return crc;
}

uint32_t crc32_ieee(const uint8_t *data, size_t size)
{
    return ~crc32_ieee_update(0xFFFFFFFFU, data, size);
}

void fill_storage_test_pattern(uint8_t *buffer,
                               size_t size,
                               uint32_t offset,
                               uint32_t seed)
{
    uint32_t value = seed ^ offset ^ 0xA5C39E17U;
    for (size_t index = 0; index < size; ++index) {
        value ^= value << 13U;
        value ^= value >> 17U;
        value ^= value << 5U;
        buffer[index] = static_cast<uint8_t>(
            value ^ (offset + static_cast<uint32_t>(index)));
    }
}

bool looks_like_fat_boot_sector(const uint8_t *sector,
                                uint32_t sector_size)
{
    if (sector_size < 512U || sector[510] != 0x55 || sector[511] != 0xAA) {
        return false;
    }
    if (std::memcmp(sector + 3, "EXFAT   ", 8) == 0) {
        return true;
    }
    const uint32_t bytes_per_sector = read_le16(sector + 11);
    const uint32_t sectors_per_cluster = sector[13];
    return bytes_per_sector == sector_size && sectors_per_cluster != 0U &&
           (sectors_per_cluster & (sectors_per_cluster - 1U)) == 0U &&
           read_le16(sector + 14) != 0U &&
           sector[16] != 0U;
}

void parse_mbr(const uint8_t *sector, StickySdRawDiagnostics &diagnostics)
{
    diagnostics.mbr_signature_valid =
        sector[510] == 0x55 && sector[511] == 0xAA;
    bool has_partition = false;
    for (size_t index = 0; index < 4; ++index) {
        const uint8_t *raw = sector + kMbrPartitionTableOffset +
                             index * kMbrPartitionEntrySize;
        StickySdMbrPartition &entry = diagnostics.partitions[index];
        entry.status = raw[0];
        entry.type = raw[4];
        entry.start_lba = read_le32(raw + 8);
        entry.sector_count = read_le32(raw + 12);
        entry.present = entry.type != 0U && entry.sector_count != 0U;
        entry.range_valid = entry.present &&
            entry.start_lba < diagnostics.sector_count &&
            entry.sector_count <=
                diagnostics.sector_count - entry.start_lba;
        has_partition = has_partition || entry.present;
        diagnostics.protective_mbr = diagnostics.protective_mbr ||
            (diagnostics.mbr_signature_valid && entry.present &&
             entry.type == 0xEE);
    }
    if (diagnostics.protective_mbr) {
        diagnostics.scheme = StickySdPartitionScheme::Gpt;
    } else if (diagnostics.mbr_signature_valid && has_partition) {
        diagnostics.scheme = StickySdPartitionScheme::Mbr;
    } else if (looks_like_fat_boot_sector(sector, diagnostics.sector_size)) {
        diagnostics.scheme = StickySdPartitionScheme::Superfloppy;
    }
}

void parse_gpt_header(uint8_t *sector, StickySdRawDiagnostics &diagnostics)
{
    diagnostics.gpt_checked = true;
    if (diagnostics.sector_size < 92U ||
        std::memcmp(sector, "EFI PART", 8) != 0) {
        return;
    }

    const uint32_t header_size = read_le32(sector + 12);
    const uint32_t stored_crc = read_le32(sector + 16);
    const uint64_t current_lba = read_le64(sector + 24);
    const uint64_t backup_lba = read_le64(sector + 32);
    diagnostics.gpt_first_usable_lba = read_le64(sector + 40);
    diagnostics.gpt_last_usable_lba = read_le64(sector + 48);
    diagnostics.gpt_partition_entry_lba = read_le64(sector + 72);
    diagnostics.gpt_partition_entry_count = read_le32(sector + 80);
    diagnostics.gpt_partition_entry_size = read_le32(sector + 84);
    diagnostics.gpt_partition_entry_crc = read_le32(sector + 88);

    bool header_crc_valid = false;
    if (header_size >= 92U && header_size <= diagnostics.sector_size) {
        std::memset(sector + 16, 0, 4);
        header_crc_valid = crc32_ieee(sector, header_size) == stored_crc;
    }
    diagnostics.gpt_header_crc_valid = header_crc_valid;
    const uint64_t entry_bytes =
        static_cast<uint64_t>(diagnostics.gpt_partition_entry_count) *
        diagnostics.gpt_partition_entry_size;
    const uint64_t entry_sectors = diagnostics.sector_size == 0U
        ? 0U
        : entry_bytes / diagnostics.sector_size +
              (entry_bytes % diagnostics.sector_size != 0U ? 1U : 0U);
    const bool fields_valid =
        current_lba == 1U && backup_lba < diagnostics.sector_count &&
        diagnostics.gpt_first_usable_lba <=
            diagnostics.gpt_last_usable_lba &&
        diagnostics.gpt_last_usable_lba < diagnostics.sector_count &&
        diagnostics.gpt_partition_entry_count != 0U &&
        diagnostics.gpt_partition_entry_size >= 128U &&
        (diagnostics.gpt_partition_entry_size % 128U) == 0U &&
        entry_sectors != 0U &&
        diagnostics.gpt_partition_entry_lba < diagnostics.sector_count &&
        entry_sectors <= diagnostics.sector_count -
            diagnostics.gpt_partition_entry_lba;
    diagnostics.gpt_valid = header_crc_valid && fields_valid;
}

bool guid_is_zero(const uint8_t *guid)
{
    for (size_t index = 0; index < 16; ++index) {
        if (guid[index] != 0U) {
            return false;
        }
    }
    return true;
}

const char *gpt_type_name(const uint8_t *guid)
{
    static constexpr uint8_t kMicrosoftBasicData[16] = {
        0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
        0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7,
    };
    static constexpr uint8_t kEfiSystem[16] = {
        0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
        0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
    };
    static constexpr uint8_t kLinuxFileSystem[16] = {
        0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
        0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4,
    };
    if (std::memcmp(guid, kMicrosoftBasicData, 16) == 0) {
        return "BASIC DATA";
    }
    if (std::memcmp(guid, kEfiSystem, 16) == 0) {
        return "EFI SYSTEM";
    }
    if (std::memcmp(guid, kLinuxFileSystem, 16) == 0) {
        return "LINUX FS";
    }
    return "GPT OTHER";
}

void add_detected_partition(StickySdRawDiagnostics &diagnostics,
                            uint32_t table_index,
                            uint8_t mbr_type,
                            uint64_t start_lba,
                            uint64_t sector_count,
                            bool from_gpt,
                            const char *type_name)
{
    ++diagnostics.total_detected_partition_count;
    if (diagnostics.detected_partition_count >=
        kStickySdDiagnosticPartitionCount) {
        return;
    }
    StickySdDetectedPartition &partition =
        diagnostics.detected_partitions[diagnostics.detected_partition_count++];
    partition.table_index = table_index;
    partition.mbr_type = mbr_type;
    partition.start_lba = start_lba;
    partition.sector_count = sector_count;
    partition.from_gpt = from_gpt;
    partition.range_valid = start_lba < diagnostics.sector_count &&
        sector_count != 0U &&
        sector_count <= diagnostics.sector_count - start_lba;
    copy_label(partition.partition_type,
               sizeof(partition.partition_type), type_name);
}

void identify_boot_file_system(const uint8_t *sector,
                               uint32_t physical_sector_size,
                               char *output,
                               size_t output_size,
                               uint32_t *cluster_size,
                               uint64_t *exfat_partition_offset,
                               uint64_t *exfat_volume_length)
{
    copy_label(output, output_size, "UNKNOWN");
    if (cluster_size != nullptr) {
        *cluster_size = 0;
    }
    if (exfat_partition_offset != nullptr) {
        *exfat_partition_offset = 0;
    }
    if (exfat_volume_length != nullptr) {
        *exfat_volume_length = 0;
    }
    if (physical_sector_size < 512U ||
        sector[510] != 0x55 || sector[511] != 0xAA) {
        return;
    }
    if (std::memcmp(sector + 3, "EXFAT   ", 8) == 0) {
        copy_label(output, output_size, "exFAT");
        if (exfat_partition_offset != nullptr) {
            *exfat_partition_offset = read_le64(sector + 64);
        }
        if (exfat_volume_length != nullptr) {
            *exfat_volume_length = read_le64(sector + 72);
        }
        const uint8_t sector_shift = sector[108];
        const uint8_t cluster_shift = sector[109];
        if (cluster_size != nullptr && sector_shift <= 12U &&
            cluster_shift <= 25U && sector_shift + cluster_shift < 32U) {
            *cluster_size = 1UL << (sector_shift + cluster_shift);
        }
        return;
    }
    if (std::memcmp(sector + 3, "NTFS    ", 8) == 0) {
        copy_label(output, output_size, "NTFS");
        return;
    }

    const uint32_t bytes_per_sector = read_le16(sector + 11);
    const uint32_t sectors_per_cluster = sector[13];
    const uint32_t reserved_sectors = read_le16(sector + 14);
    const uint32_t fat_count = sector[16];
    const uint32_t root_entries = read_le16(sector + 17);
    const uint32_t total_sectors_16 = read_le16(sector + 19);
    const uint32_t fat_sectors_16 = read_le16(sector + 22);
    const uint32_t total_sectors = total_sectors_16 != 0U
        ? total_sectors_16 : read_le32(sector + 32);
    const uint32_t fat_sectors = fat_sectors_16 != 0U
        ? fat_sectors_16 : read_le32(sector + 36);
    const bool valid_bpb =
        bytes_per_sector == physical_sector_size &&
        sectors_per_cluster != 0U &&
        (sectors_per_cluster & (sectors_per_cluster - 1U)) == 0U &&
        reserved_sectors != 0U && fat_count != 0U && fat_sectors != 0U;
    if (!valid_bpb) {
        return;
    }
    const uint32_t root_directory_sectors =
        ((root_entries * 32U) + bytes_per_sector - 1U) / bytes_per_sector;
    const uint64_t non_data_sectors = reserved_sectors +
        static_cast<uint64_t>(fat_count) * fat_sectors +
        root_directory_sectors;
    if (total_sectors <= non_data_sectors) {
        return;
    }
    const uint64_t cluster_count =
        (total_sectors - non_data_sectors) / sectors_per_cluster;
    copy_label(output, output_size,
               cluster_count < 4085U ? "FAT12" :
               (cluster_count < 65525U ? "FAT16" : "FAT32"));
    if (cluster_size != nullptr &&
        bytes_per_sector <=
            std::numeric_limits<uint32_t>::max() / sectors_per_cluster) {
        *cluster_size = bytes_per_sector * sectors_per_cluster;
    }
}

const char *mbr_type_name(uint8_t type)
{
    switch (type) {
    case 0x06: return "FAT16";
    case 0x07: return "NTFS/exFAT";
    case 0x0B:
    case 0x0C: return "FAT32";
    case 0x0E: return "FAT16 LBA";
    case 0x83: return "LINUX";
    default: return "MBR OTHER";
    }
}

esp_err_t parse_gpt_entries(sdmmc_card_t *card,
                            uint8_t *sector,
                            StickySdRawDiagnostics &diagnostics)
{
    constexpr uint64_t kMaximumEntryTableBytes = 1024ULL * 1024ULL;
    const uint64_t table_bytes =
        static_cast<uint64_t>(diagnostics.gpt_partition_entry_count) *
        diagnostics.gpt_partition_entry_size;
    if (!diagnostics.gpt_valid || table_bytes == 0U ||
        table_bytes > kMaximumEntryTableBytes) {
        if (table_bytes > kMaximumEntryTableBytes) {
            diagnostics.gpt_valid = false;
        }
        return ESP_OK;
    }

    uint64_t remaining = table_bytes;
    uint64_t lba = diagnostics.gpt_partition_entry_lba;
    uint32_t entry_index = 0;
    uint32_t crc = 0xFFFFFFFFU;
    while (remaining > 0U) {
        const esp_err_t result = sdmmc_read_sectors(
            card, sector, static_cast<size_t>(lba), 1);
        if (result != ESP_OK) {
            return result;
        }
        const size_t bytes_this_sector = static_cast<size_t>(
            std::min<uint64_t>(remaining, diagnostics.sector_size));
        crc = crc32_ieee_update(crc, sector, bytes_this_sector);

        if (diagnostics.gpt_partition_entry_size <=
                diagnostics.sector_size &&
            diagnostics.sector_size %
                diagnostics.gpt_partition_entry_size == 0U) {
            for (size_t offset = 0;
                 offset + diagnostics.gpt_partition_entry_size <=
                     bytes_this_sector &&
                 entry_index < diagnostics.gpt_partition_entry_count;
                 offset += diagnostics.gpt_partition_entry_size,
                 ++entry_index) {
                const uint8_t *entry = sector + offset;
                if (guid_is_zero(entry)) {
                    continue;
                }
                const uint64_t first_lba = read_le64(entry + 32);
                const uint64_t last_lba = read_le64(entry + 40);
                const uint64_t sector_count = last_lba >= first_lba
                    ? last_lba - first_lba + 1U : 0U;
                add_detected_partition(
                    diagnostics, entry_index + 1U, 0U,
                    first_lba, sector_count, true, gpt_type_name(entry));
            }
        }
        remaining -= bytes_this_sector;
        ++lba;
    }
    diagnostics.gpt_entries_crc_checked = true;
    diagnostics.gpt_entries_crc_valid =
        ~crc == diagnostics.gpt_partition_entry_crc;
    diagnostics.gpt_valid = diagnostics.gpt_valid &&
        diagnostics.gpt_entries_crc_valid;
    return ESP_OK;
}

esp_err_t analyze_detected_partitions(sdmmc_card_t *card,
                                      uint8_t *sector,
                                      StickySdRawDiagnostics &diagnostics)
{
    if (diagnostics.scheme == StickySdPartitionScheme::Gpt) {
        const esp_err_t result = parse_gpt_entries(card, sector, diagnostics);
        if (result != ESP_OK) {
            return result;
        }
    } else if (diagnostics.scheme == StickySdPartitionScheme::Mbr) {
        for (size_t index = 0; index < 4; ++index) {
            const StickySdMbrPartition &entry = diagnostics.partitions[index];
            if (entry.present && entry.type != 0xEE) {
                add_detected_partition(
                    diagnostics, static_cast<uint32_t>(index + 1U),
                    entry.type, entry.start_lba, entry.sector_count, false,
                    mbr_type_name(entry.type));
            }
        }
    } else if (diagnostics.scheme == StickySdPartitionScheme::Superfloppy) {
        add_detected_partition(diagnostics, 1U, 0U, 0U,
                               diagnostics.sector_count, false,
                               "WHOLE CARD");
    }

    for (size_t index = 0; index < diagnostics.detected_partition_count;
         ++index) {
        StickySdDetectedPartition &partition =
            diagnostics.detected_partitions[index];
        if (!partition.range_valid) {
            continue;
        }
        const esp_err_t result = sdmmc_read_sectors(
            card, sector, static_cast<size_t>(partition.start_lba), 1);
        if (result != ESP_OK) {
            return result;
        }
        partition.boot_sector_read = true;
        identify_boot_file_system(sector, diagnostics.sector_size,
                                  partition.file_system,
                                  sizeof(partition.file_system),
                                  &partition.cluster_size,
                                  &partition.exfat_partition_offset,
                                  &partition.exfat_volume_length);
        if (std::strcmp(diagnostics.detected_file_system, "UNKNOWN") == 0 &&
            std::strcmp(partition.file_system, "UNKNOWN") != 0) {
            copy_label(diagnostics.detected_file_system,
                       sizeof(diagnostics.detected_file_system),
                       partition.file_system);
        }
    }
    return ESP_OK;
}

bool is_power_of_two(uint32_t value)
{
    return value != 0 && (value & (value - 1U)) == 0;
}

void copy_label(char *destination, size_t size, const char *source)
{
    std::snprintf(destination, size, "%s", source);
}

esp_err_t set_card_power(bool enabled)
{
    return gpio_set_level(
        static_cast<gpio_num_t>(PIN_SD_EN), enabled ? 1 : 0);
}

bool card_is_inserted()
{
    return gpio_get_level(static_cast<gpio_num_t>(PIN_SD_DETECT)) == 0;
}

uint32_t find_volume_sector(const uint8_t *sector,
                            uint64_t card_sector_count,
                            StickySdCardInfo &info)
{
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        return 0;
    }

    for (size_t index = 0; index < 4; ++index) {
        const uint8_t *entry = sector + kMbrPartitionTableOffset +
                               index * kMbrPartitionEntrySize;
        const uint8_t status = entry[0];
        const uint8_t type = entry[4];
        const uint32_t first_sector = read_le32(entry + 8);
        const uint32_t sector_count = read_le32(entry + 12);
        const bool valid_status = status == 0x00 || status == 0x80;
        const bool within_card =
            first_sector < card_sector_count &&
            sector_count <= card_sector_count - first_sector;

        if (valid_status && type != 0 && first_sector != 0 &&
            sector_count != 0 && within_card) {
            copy_label(info.partition, sizeof(info.partition),
                       type == 0xEE ? "GPT" : "MBR");
            return first_sector;
        }
    }

    copy_label(info.partition, sizeof(info.partition), "SUPERFLOPPY");
    return 0;
}

void decode_file_system(const uint8_t *boot_sector,
                        uint32_t physical_sector_size,
                        StickySdCardInfo &info)
{
    if (std::memcmp(boot_sector + 3, "EXFAT   ", 8) == 0) {
        copy_label(info.file_system, sizeof(info.file_system), "exFAT");
        const uint8_t sector_shift = boot_sector[108];
        const uint8_t cluster_shift = boot_sector[109];
        if (sector_shift <= 12 && cluster_shift <= 25 &&
            sector_shift + cluster_shift < 32) {
            info.cluster_size = 1UL << (sector_shift + cluster_shift);
        }
        return;
    }

    const uint32_t bytes_per_sector = read_le16(boot_sector + 11);
    const uint32_t sectors_per_cluster = boot_sector[13];
    const uint32_t reserved_sectors = read_le16(boot_sector + 14);
    const uint32_t fat_count = boot_sector[16];
    const uint32_t root_entries = read_le16(boot_sector + 17);
    const uint32_t total_sectors_16 = read_le16(boot_sector + 19);
    const uint32_t fat_sectors_16 = read_le16(boot_sector + 22);
    const uint32_t total_sectors = total_sectors_16 != 0
                                       ? total_sectors_16
                                       : read_le32(boot_sector + 32);
    const uint32_t fat_sectors = fat_sectors_16 != 0
                                     ? fat_sectors_16
                                     : read_le32(boot_sector + 36);

    const bool valid_bpb =
        is_power_of_two(bytes_per_sector) && bytes_per_sector >= 512 &&
        bytes_per_sector <= 4096 &&
        bytes_per_sector == physical_sector_size &&
        is_power_of_two(sectors_per_cluster) &&
        reserved_sectors != 0 && fat_count != 0 && fat_sectors != 0;
    if (!valid_bpb) {
        return;
    }

    const uint32_t root_directory_sectors =
        ((root_entries * 32U) + (bytes_per_sector - 1U)) / bytes_per_sector;
    const uint64_t non_data_sectors =
        static_cast<uint64_t>(reserved_sectors) +
        static_cast<uint64_t>(fat_count) * fat_sectors +
        root_directory_sectors;
    if (total_sectors <= non_data_sectors) {
        return;
    }

    const uint64_t cluster_count =
        (total_sectors - non_data_sectors) / sectors_per_cluster;
    const char *type = cluster_count < 4085
                           ? "FAT12"
                           : (cluster_count < 65525 ? "FAT16" : "FAT32");
    copy_label(info.file_system, sizeof(info.file_system), type);
    info.cluster_size = bytes_per_sector * sectors_per_cluster;
}

esp_err_t read_disk_layout(sdmmc_card_t *card, StickySdCardInfo &info)
{
    const size_t sector_size = card->csd.sector_size;
    if (sector_size < 512) {
        return ESP_ERR_INVALID_SIZE;
    }

    auto *sector = static_cast<uint8_t *>(
        heap_caps_malloc(sector_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (sector == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = sdmmc_read_sectors(card, sector, 0, 1);
    if (result == ESP_OK) {
        const uint32_t volume_sector =
            find_volume_sector(sector, info.sector_count, info);
        if (volume_sector != 0) {
            result = sdmmc_read_sectors(card, sector, volume_sector, 1);
        }
        if (result == ESP_OK) {
            decode_file_system(sector, info.sector_size, info);
        }
    }

    heap_caps_free(sector);
    return result;
}

}  // namespace

esp_err_t sticky_sdcard_init()
{
    if (s_initialized) {
        return ESP_OK;
    }

    gpio_config_t output_config = {};
    output_config.pin_bit_mask = (1ULL << PIN_SD_EN) | (1ULL << PIN_SD_CS);
    output_config.mode = GPIO_MODE_OUTPUT;
    output_config.pull_up_en = GPIO_PULLUP_DISABLE;
    output_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    output_config.intr_type = GPIO_INTR_DISABLE;
    esp_err_t result = gpio_config(&output_config);
    if (result != ESP_OK) {
        return result;
    }

    result = gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (result != ESP_OK) {
        return result;
    }
    result = set_card_power(false);
    if (result != ESP_OK) {
        return result;
    }

    gpio_config_t detect_config = {};
    detect_config.pin_bit_mask = 1ULL << PIN_SD_DETECT;
    detect_config.mode = GPIO_MODE_INPUT;
    detect_config.pull_up_en = GPIO_PULLUP_ENABLE;
    detect_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    detect_config.intr_type = GPIO_INTR_DISABLE;
    result = gpio_config(&detect_config);
    if (result == ESP_OK) {
        s_initialized = true;
        ESP_LOGI(kTag, "Interface ready on shared SPI2");
    }
    return result;
}

bool sticky_sdcard_is_inserted()
{
    return s_initialized && card_is_inserted();
}

esp_err_t sticky_sdcard_get_info(StickySdCardInfo &info)
{
    info = {};
    if (!s_initialized) {
        info.error = ESP_ERR_INVALID_STATE;
        return info.error;
    }

    info.inserted = card_is_inserted();
    if (!info.inserted) {
        set_card_power(false);
        info.error = ESP_ERR_NOT_FOUND;
        ESP_LOGW(kTag, "No MicroSD card detected");
        return info.error;
    }

    esp_err_t result = gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (result == ESP_OK) {
        result = set_card_power(true);
    }
    if (result != ESP_OK) {
        info.error = result;
        return result;
    }
    vTaskDelay(kPowerOnDelay);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = static_cast<gpio_num_t>(PIN_SD_CS);
    slot_config.host_id = SPI2_HOST;
    slot_config.gpio_cd = GPIO_NUM_NC;
    slot_config.gpio_wp = GPIO_NUM_NC;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = nullptr;
    result = esp_vfs_fat_sdspi_mount(
        kMountPoint, &host, &slot_config, &mount_config, &card);
    if (result != ESP_OK) {
        info.error = result;
        ESP_LOGE(kTag, "MicroSD mount failed: %s", esp_err_to_name(result));
        gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
        if (!card_is_inserted()) {
            set_card_power(false);
        }
        return result;
    }

    info.mounted = true;
    info.sector_size = card->csd.sector_size;
    info.sector_count = card->csd.capacity;
    copy_label(info.card_type, sizeof(info.card_type),
               sd_card_capacity_class(*card));

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    result = esp_vfs_fat_info(kMountPoint, &total_bytes, &free_bytes);
    if (result == ESP_OK) {
        info.total_bytes = total_bytes;
        info.free_bytes = std::min(free_bytes, total_bytes);
        info.used_bytes = total_bytes - info.free_bytes;
        const esp_err_t layout_result = read_disk_layout(card, info);
        if (layout_result != ESP_OK) {
            ESP_LOGW(kTag, "Partition/BPB read failed: %s",
                     esp_err_to_name(layout_result));
        }
    }

    const esp_err_t unmount_result =
        esp_vfs_fat_sdcard_unmount(kMountPoint, card);
    if (result == ESP_OK && unmount_result != ESP_OK) {
        result = unmount_result;
    }

    gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (!card_is_inserted()) {
        set_card_power(false);
    }
    info.inserted = card_is_inserted();
    info.error = result;
    ESP_LOGI(kTag, "MicroSD query complete; shared SPI2 is ready for display");
    return result;
}

esp_err_t sticky_sdcard_get_raw_diagnostics(
    StickySdRawDiagnostics &diagnostics)
{
    diagnostics = {};
    if (!s_initialized) {
        diagnostics.error = ESP_ERR_INVALID_STATE;
        return diagnostics.error;
    }

    diagnostics.inserted = card_is_inserted();
    if (!diagnostics.inserted) {
        set_card_power(false);
        diagnostics.error = ESP_ERR_NOT_FOUND;
        ESP_LOGW(kTag, "Raw diagnostics: no MicroSD card detected");
        return diagnostics.error;
    }

    esp_err_t result = gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (result == ESP_OK) {
        result = set_card_power(true);
    }
    if (result != ESP_OK) {
        diagnostics.error = result;
        return result;
    }
    vTaskDelay(kPowerOnDelay);

    sdspi_dev_handle_t device = -1;
    bool device_added = false;
    sdmmc_card_t card = {};
    uint8_t *sector = nullptr;

    result = sdspi_host_init();
    if (result == ESP_OK) {
        sdspi_device_config_t device_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        device_config.host_id = SPI2_HOST;
        device_config.gpio_cs = static_cast<gpio_num_t>(PIN_SD_CS);
        device_config.gpio_cd = GPIO_NUM_NC;
        device_config.gpio_wp = GPIO_NUM_NC;
        result = sdspi_host_init_device(&device_config, &device);
        device_added = result == ESP_OK;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    if (result == ESP_OK) {
        host.slot = device;
        result = sdmmc_card_init(&host, &card);
    }
    if (result == ESP_OK) {
        diagnostics.card_initialized = true;
        diagnostics.sector_size = card.csd.sector_size;
        diagnostics.sector_count = card.csd.capacity;
        diagnostics.capacity_bytes =
            diagnostics.sector_count * diagnostics.sector_size;
        copy_label(diagnostics.card_type, sizeof(diagnostics.card_type),
                   sd_card_capacity_class(card));
        diagnostics.card.ocr = card.ocr;
        diagnostics.card.host_limit_khz =
            static_cast<uint32_t>(std::max(card.host.max_freq_khz, 0));
        diagnostics.card.actual_clock_khz = card.real_freq_khz;
        diagnostics.card.manufacturer_id =
            static_cast<uint8_t>(card.cid.mfg_id);
        diagnostics.card.oem_id = static_cast<uint16_t>(card.cid.oem_id);
        std::memcpy(diagnostics.card.product_name, card.cid.name,
                    sizeof(diagnostics.card.product_name) - 1U);
        diagnostics.card.product_name[
            sizeof(diagnostics.card.product_name) - 1U] = '\0';
        diagnostics.card.product_revision =
            static_cast<uint8_t>(card.cid.revision);
        diagnostics.card.product_serial =
            static_cast<uint32_t>(card.cid.serial);
        diagnostics.card.manufacturing_date =
            static_cast<uint16_t>(card.cid.date);
        diagnostics.card.csd_version = card.csd.csd_ver;
        const unsigned read_block_length_code =
            static_cast<unsigned>(std::max(card.csd.read_block_len, 0));
        diagnostics.card.read_block_size =
            read_block_length_code < 32U
                ? (1UL << read_block_length_code)
                : 0U;
        diagnostics.card.command_classes =
            static_cast<uint32_t>(std::max(card.csd.card_command_class, 0));
        diagnostics.card.advertised_transfer_hz =
            static_cast<uint32_t>(std::max(card.csd.tr_speed, 0));
        diagnostics.card.sd_spec = static_cast<uint8_t>(card.scr.sd_spec);
        diagnostics.card.supported_bus_widths =
            static_cast<uint8_t>(card.scr.bus_width);
        diagnostics.card.current_bus_width =
            static_cast<uint8_t>(card.ssr.cur_bus_width ? 4U : 1U);
        diagnostics.card.allocation_unit_kib = card.ssr.alloc_unit_kb;
        diagnostics.card.erase_size_au = card.ssr.erase_size_au;
        diagnostics.card.discard_supported = card.ssr.discard_support != 0U;
        diagnostics.card.full_user_area_erase_supported =
            card.ssr.fule_support != 0U;
        if (diagnostics.sector_size < 512U) {
            result = ESP_ERR_INVALID_SIZE;
        }
    }
    if (result == ESP_OK) {
        sector = static_cast<uint8_t *>(heap_caps_malloc(
            diagnostics.sector_size, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
        if (sector == nullptr) {
            result = ESP_ERR_NO_MEM;
        }
    }
    if (result == ESP_OK) {
        result = sdmmc_read_sectors(&card, sector, 0, 1);
        if (result == ESP_OK) {
            diagnostics.sector0_read = true;
            parse_mbr(sector, diagnostics);
        }
    }
    if (result == ESP_OK && diagnostics.protective_mbr) {
        result = sdmmc_read_sectors(&card, sector, 1, 1);
        if (result == ESP_OK) {
            parse_gpt_header(sector, diagnostics);
        }
    }
    if (result == ESP_OK) {
        result = analyze_detected_partitions(&card, sector, diagnostics);
    }

    if (sector != nullptr) {
        heap_caps_free(sector);
    }
    if (device_added) {
        const esp_err_t remove_result = sdspi_host_remove_device(device);
        if (result == ESP_OK && remove_result != ESP_OK) {
            result = remove_result;
        }
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (!card_is_inserted()) {
        set_card_power(false);
    }
    diagnostics.inserted = card_is_inserted();
    diagnostics.error = result;

    if (diagnostics.card_initialized) {
        ESP_LOGI(kTag,
                 "Raw: card=%s sectors=%llu sector_size=%lu capacity=%llu",
                 diagnostics.card_type,
                 static_cast<unsigned long long>(diagnostics.sector_count),
                 static_cast<unsigned long>(diagnostics.sector_size),
                 static_cast<unsigned long long>(diagnostics.capacity_bytes));
    }
    ESP_LOGI(kTag, "Raw: LBA0=%s MBR_SIG=%s scheme=%s protective=%s",
             diagnostics.sector0_read ? "READ" : "FAILED",
             diagnostics.mbr_signature_valid ? "VALID" : "INVALID",
             sdcard_internal::partition_scheme_name(diagnostics.scheme),
             diagnostics.protective_mbr ? "YES" : "NO");
    for (size_t index = 0; index < 4; ++index) {
        const StickySdMbrPartition &entry = diagnostics.partitions[index];
        const bool empty_slot = entry.status == 0U && entry.type == 0U &&
                                entry.start_lba == 0U &&
                                entry.sector_count == 0U;
        if (empty_slot) {
            continue;
        }
        ESP_LOGI(kTag,
                 "Raw: partition %u type=0x%02X start=%lu sectors=%lu range=%s",
                 static_cast<unsigned>(index + 1U),
                 static_cast<unsigned>(entry.type),
                 static_cast<unsigned long>(entry.start_lba),
                 static_cast<unsigned long>(entry.sector_count),
                 entry.range_valid ? "VALID" : "INVALID/EMPTY");
    }
    if (diagnostics.gpt_checked) {
        ESP_LOGI(kTag,
                 "Raw: GPT=%s header_crc=%s entries_crc=%s usable=%llu..%llu entries_lba=%llu count=%lu size=%lu",
                 diagnostics.gpt_valid ? "VALID" : "INVALID",
                 diagnostics.gpt_header_crc_valid ? "VALID" : "INVALID",
                 diagnostics.gpt_entries_crc_checked
                     ? (diagnostics.gpt_entries_crc_valid
                            ? "VALID" : "INVALID")
                     : "NOT CHECKED",
                 static_cast<unsigned long long>(
                     diagnostics.gpt_first_usable_lba),
                 static_cast<unsigned long long>(
                     diagnostics.gpt_last_usable_lba),
                 static_cast<unsigned long long>(
                     diagnostics.gpt_partition_entry_lba),
                 static_cast<unsigned long>(
                     diagnostics.gpt_partition_entry_count),
                 static_cast<unsigned long>(
                     diagnostics.gpt_partition_entry_size));
    }
    ESP_LOGI(kTag, "Raw: detected partitions=%u stored=%u filesystem=%s",
             static_cast<unsigned>(
                 diagnostics.total_detected_partition_count),
             static_cast<unsigned>(diagnostics.detected_partition_count),
             diagnostics.detected_file_system);
    for (size_t index = 0; index < diagnostics.detected_partition_count;
         ++index) {
        const StickySdDetectedPartition &partition =
            diagnostics.detected_partitions[index];
        ESP_LOGI(kTag,
                 "Raw: actual partition %lu source=%s type=%s start=%llu sectors=%llu range=%s fs=%s boot=%s",
                 static_cast<unsigned long>(partition.table_index),
                 partition.from_gpt ? "GPT" : "MBR/RAW",
                 partition.partition_type,
                 static_cast<unsigned long long>(partition.start_lba),
                 static_cast<unsigned long long>(partition.sector_count),
                 partition.range_valid ? "VALID" : "INVALID",
                 partition.file_system,
                 partition.boot_sector_read ? "READ" : "NOT READ");
    }
    ESP_LOGI(kTag, "Raw diagnostics complete: %s; SPI2 released",
             esp_err_to_name(result));
    return result;
}

esp_err_t sticky_sdcard_run_storage_test(StickySdStorageTestResult &test)
{
    constexpr size_t kTestBytes = 32U * 1024U;
    constexpr size_t kChunkBytes = 1024U;
    test = {};
    test.attempted = true;

    auto set_failure = [&test](const char *reason, esp_err_t error) {
        if (std::strcmp(test.failure_reason, "NOT STARTED") == 0 ||
            test.failure_reason[0] == '\0') {
            std::snprintf(test.failure_reason, sizeof(test.failure_reason),
                          "%s", reason);
        }
        test.error = error;
    };

    if (!s_initialized) {
        set_failure("SD INTERFACE NOT READY", ESP_ERR_INVALID_STATE);
        ESP_LOGE(kTag, "Storage test aborted: %s",
                 test.failure_reason);
        return test.error;
    }
    if (!card_is_inserted()) {
        set_card_power(false);
        set_failure("NO SD CARD", ESP_ERR_NOT_FOUND);
        ESP_LOGE(kTag, "Storage test aborted: %s",
                 test.failure_reason);
        return test.error;
    }

    esp_err_t result = gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (result == ESP_OK) result = set_card_power(true);
    if (result != ESP_OK) {
        set_failure("CARD POWER FAILED", result);
        ESP_LOGE(kTag, "Storage test aborted: %s (%s)",
                 test.failure_reason, esp_err_to_name(result));
        return result;
    }
    vTaskDelay(kPowerOnDelay);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = static_cast<gpio_num_t>(PIN_SD_CS);
    slot_config.host_id = SPI2_HOST;
    slot_config.gpio_cd = GPIO_NUM_NC;
    slot_config.gpio_wp = GPIO_NUM_NC;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    // Keep this identical to the proven SD Info/FILES mount configuration.
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = nullptr;
    result = esp_vfs_fat_sdspi_mount(
        kMountPoint, &host, &slot_config, &mount_config, &card);
    if (result != ESP_OK) {
        set_failure("CARD CANNOT BE MOUNTED", result);
        ESP_LOGE(kTag, "Storage test mount failed: %s",
                 esp_err_to_name(result));
        ESP_LOGE(kTag,
                 "Storage test complete: FAILED | write=NOT RUN read=NOT RUN verify=NOT RUN cleanup=NOT NEEDED reason=%s",
                 test.failure_reason);
        gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
        if (!card_is_inserted()) set_card_power(false);
        return result;
    }
    test.mount_pass = true;
    ESP_LOGI(kTag, "Storage test: mount PASS");

    char test_path[80] = {};
    int descriptor = -1;
    uint32_t pattern_seed = 0;
    for (size_t attempt = 0; attempt < 8U; ++attempt) {
        pattern_seed = esp_random();
        std::snprintf(test_path, sizeof(test_path),
                      "%s/.sd_debugger_test_%08lX.tmp",
                      kMountPoint,
                      static_cast<unsigned long>(pattern_seed));
        descriptor = open(test_path, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (descriptor >= 0 || errno != EEXIST) break;
    }
    if (descriptor < 0) {
        result = filesystem_error(errno);
        set_failure(errno == EEXIST ? "TEMP NAME COLLISION"
                                    : "TEMP FILE CREATE FAILED",
                    result);
        ESP_LOGE(kTag, "Storage test create FAIL: errno=%d (%s)",
                 errno, std::strerror(errno));
    } else {
        test.create_pass = true;
        test.cleanup_attempted = true;
        ESP_LOGI(kTag, "Storage test: created exclusive file %s", test_path);
    }

    // app_main owns a large AppState on an 8 KiB task stack. Keeping three
    // 1 KiB buffers as local arrays exhausted the remaining stack while the
    // deeper sdmmc_card_init() call was running. Allocate them after mount so
    // the Storage Test uses the same small-stack mount path as other SD APIs.
    uint8_t *test_buffers = nullptr;
    if (result == ESP_OK) {
        test_buffers = static_cast<uint8_t *>(heap_caps_malloc(
            3U * kChunkBytes, MALLOC_CAP_8BIT));
        if (test_buffers == nullptr) {
            result = ESP_ERR_NO_MEM;
            set_failure("TEST BUFFER ALLOCATION FAILED", result);
        }
    }
    uint8_t *write_buffer = test_buffers;
    uint8_t *read_buffer = test_buffers != nullptr
        ? test_buffers + kChunkBytes : nullptr;
    uint8_t *expected_buffer = test_buffers != nullptr
        ? test_buffers + 2U * kChunkBytes : nullptr;
    uint32_t expected_crc = 0xFFFFFFFFU;
    if (result == ESP_OK) {
        for (size_t offset = 0; offset < kTestBytes && result == ESP_OK;
             offset += kChunkBytes) {
            if (!card_is_inserted()) {
                result = ESP_ERR_NOT_FOUND;
                set_failure("CARD REMOVED DURING WRITE", result);
                break;
            }
            fill_storage_test_pattern(write_buffer, kChunkBytes,
                                      static_cast<uint32_t>(offset),
                                      pattern_seed);
            expected_crc = crc32_ieee_update(
                expected_crc, write_buffer, kChunkBytes);
            size_t written = 0;
            while (written < kChunkBytes) {
                const ssize_t count = write(
                    descriptor, write_buffer + written,
                    kChunkBytes - written);
                if (count <= 0) {
                    result = filesystem_error(errno);
                    set_failure("WRITE FAILED", result);
                    break;
                }
                written += static_cast<size_t>(count);
            }
        }
        if (result == ESP_OK && fsync(descriptor) != 0) {
            result = filesystem_error(errno);
            set_failure("SAVE/FSYNC FAILED", result);
        }
        if (close(descriptor) != 0 && result == ESP_OK) {
            result = filesystem_error(errno);
            set_failure("FILE CLOSE FAILED", result);
        }
        descriptor = -1;
        if (result == ESP_OK) {
            test.write_pass = true;
            test.expected_crc32 = ~expected_crc;
            ESP_LOGI(kTag, "Storage test: write PASS (%u bytes, CRC32=%08lX)",
                     static_cast<unsigned>(kTestBytes),
                     static_cast<unsigned long>(test.expected_crc32));
        }
    }

    if (result == ESP_OK) {
        descriptor = open(test_path, O_RDONLY);
        if (descriptor < 0) {
            result = filesystem_error(errno);
            set_failure("REOPEN FOR READ FAILED", result);
        }
    }
    uint32_t actual_crc = 0xFFFFFFFFU;
    bool content_matches = true;
    if (result == ESP_OK) {
        for (size_t offset = 0; offset < kTestBytes && result == ESP_OK;
             offset += kChunkBytes) {
            if (!card_is_inserted()) {
                result = ESP_ERR_NOT_FOUND;
                set_failure("CARD REMOVED DURING READ", result);
                break;
            }
            size_t received = 0;
            while (received < kChunkBytes) {
                const ssize_t count = read(
                    descriptor, read_buffer + received,
                    kChunkBytes - received);
                if (count <= 0) {
                    result = count == 0 ? ESP_ERR_INVALID_SIZE
                                        : filesystem_error(errno);
                    set_failure("READ FAILED OR SHORT", result);
                    break;
                }
                received += static_cast<size_t>(count);
            }
            if (result != ESP_OK) break;
            fill_storage_test_pattern(expected_buffer, kChunkBytes,
                                      static_cast<uint32_t>(offset),
                                      pattern_seed);
            content_matches = content_matches &&
                std::memcmp(read_buffer, expected_buffer, kChunkBytes) == 0;
            actual_crc = crc32_ieee_update(
                actual_crc, read_buffer, kChunkBytes);
        }
        if (close(descriptor) != 0 && result == ESP_OK) {
            result = filesystem_error(errno);
            set_failure("READ FILE CLOSE FAILED", result);
        }
        descriptor = -1;
        if (result == ESP_OK) {
            test.read_pass = true;
            test.actual_crc32 = ~actual_crc;
            test.verify_pass = content_matches &&
                test.actual_crc32 == test.expected_crc32;
            test.bytes_tested = static_cast<uint32_t>(kTestBytes);
            if (!test.verify_pass) {
                result = ESP_ERR_INVALID_CRC;
                set_failure("DATA VERIFY MISMATCH", result);
            }
            ESP_LOGI(kTag, "Storage test: read PASS; verify %s (CRC32=%08lX)",
                     test.verify_pass ? "PASS" : "FAIL",
                     static_cast<unsigned long>(test.actual_crc32));
        }
    }

    if (descriptor >= 0) close(descriptor);
    if (test.create_pass) {
        test.cleanup_attempted = true;
        if (unlink(test_path) == 0) {
            test.cleanup_pass = true;
            ESP_LOGI(kTag, "Storage test: cleanup PASS");
        } else {
            const esp_err_t cleanup_error = filesystem_error(errno);
            ESP_LOGE(kTag, "Storage test cleanup FAIL: errno=%d (%s)",
                     errno, std::strerror(errno));
            if (result == ESP_OK) {
                result = cleanup_error;
                set_failure("TEMP FILE CLEANUP FAILED", result);
            }
        }
    }

    if (test_buffers != nullptr) {
        heap_caps_free(test_buffers);
        test_buffers = nullptr;
    }

    const esp_err_t unmount_result =
        esp_vfs_fat_sdcard_unmount(kMountPoint, card);
    if (result == ESP_OK && unmount_result != ESP_OK) {
        result = unmount_result;
        set_failure("CARD UNMOUNT FAILED", result);
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (!card_is_inserted()) set_card_power(false);

    test.passed = result == ESP_OK && test.mount_pass && test.create_pass &&
        test.write_pass && test.read_pass && test.verify_pass &&
        test.cleanup_pass;
    test.error = result;
    if (test.passed) {
        std::snprintf(test.failure_reason, sizeof(test.failure_reason),
                      "NONE");
    }
    ESP_LOGI(kTag,
             "Storage test complete: %s | write=%s read=%s verify=%s cleanup=%s reason=%s error=%s",
             test.passed ? "PASSED" : "FAILED",
             test.write_pass ? "PASS" : "FAIL",
             test.read_pass ? "PASS" : "FAIL",
             test.verify_pass ? "PASS" : "FAIL",
             test.cleanup_pass ? "PASS" : "FAIL",
             test.failure_reason,
             esp_err_to_name(test.error));
    ESP_LOGI(kTag, "Storage test finished; shared SPI2 is ready for display");
    return result;
}

esp_err_t sticky_sdcard_clear_contents(StickySdClearCardResult &clear_result)
{
    clear_result = {};
    clear_result.attempted = true;
    auto set_failure = [&clear_result](const char *reason, esp_err_t error) {
        if (std::strcmp(clear_result.failure_reason, "NOT STARTED") == 0 ||
            clear_result.failure_reason[0] == '\0') {
            std::snprintf(clear_result.failure_reason,
                          sizeof(clear_result.failure_reason), "%s", reason);
        }
        clear_result.error = error;
    };

    if (!s_initialized) {
        set_failure("SD INTERFACE NOT READY", ESP_ERR_INVALID_STATE);
        ESP_LOGE(kTag, "Clear Card refused: %s",
                 clear_result.failure_reason);
        return clear_result.error;
    }
    if (!card_is_inserted()) {
        set_card_power(false);
        set_failure("NO SD CARD", ESP_ERR_NOT_FOUND);
        ESP_LOGE(kTag, "Clear Card refused: %s",
                 clear_result.failure_reason);
        return clear_result.error;
    }

    esp_err_t result = gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (result == ESP_OK) result = set_card_power(true);
    if (result != ESP_OK) {
        set_failure("CARD POWER FAILED", result);
        ESP_LOGE(kTag, "Clear Card refused: %s (%s)",
                 clear_result.failure_reason, esp_err_to_name(result));
        return result;
    }
    vTaskDelay(kPowerOnDelay);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = static_cast<gpio_num_t>(PIN_SD_CS);
    slot_config.host_id = SPI2_HOST;
    slot_config.gpio_cd = GPIO_NUM_NC;
    slot_config.gpio_wp = GPIO_NUM_NC;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = nullptr;
    result = esp_vfs_fat_sdspi_mount(
        kMountPoint, &host, &slot_config, &mount_config, &card);
    if (result != ESP_OK) {
        set_failure("CARD CANNOT BE MOUNTED", result);
        ESP_LOGE(kTag, "Clear Card refused: mount failed: %s",
                 esp_err_to_name(result));
        gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
        if (!card_is_inserted()) set_card_power(false);
        return result;
    }
    clear_result.mount_pass = true;
    ESP_LOGW(kTag, "Clear Card started: deleting contents below %s",
             kMountPoint);

    while (result == ESP_OK) {
        if (!card_is_inserted()) {
            result = ESP_ERR_NOT_FOUND;
            set_failure("CARD REMOVED DURING CLEAR", result);
            break;
        }

        DIR *root = opendir(kMountPoint);
        if (root == nullptr) {
            result = filesystem_error(errno);
            set_failure("OPEN CARD ROOT FAILED", result);
            break;
        }

        char *child_path = nullptr;
        bool child_is_directory = false;
        errno = 0;
        while (child_path == nullptr) {
            dirent *entry = readdir(root);
            if (entry == nullptr) {
                if (errno != 0) {
                    result = filesystem_error(errno);
                    set_failure("READ CARD ROOT FAILED", result);
                }
                break;
            }
            if (std::strcmp(entry->d_name, ".") == 0 ||
                std::strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            if (!valid_entry_name(entry->d_name)) {
                result = ESP_ERR_INVALID_ARG;
                set_failure("UNSAFE ROOT ENTRY REJECTED", result);
                break;
            }

            const size_t root_length = std::strlen(kMountPoint);
            const size_t name_length = std::strlen(entry->d_name);
            const size_t path_size = root_length + 1U + name_length + 1U;
            if (path_size > kMaximumSdPathSize) {
                result = ESP_ERR_INVALID_SIZE;
                set_failure("ROOT ENTRY PATH TOO LONG", result);
                break;
            }
            child_path = static_cast<char *>(std::malloc(path_size));
            if (child_path == nullptr) {
                result = ESP_ERR_NO_MEM;
                set_failure("CLEAR PATH ALLOCATION FAILED", result);
                break;
            }
            std::snprintf(child_path, path_size, "%s/%s",
                          kMountPoint, entry->d_name);
            struct stat status = {};
            if (stat(child_path, &status) != 0) {
                result = filesystem_error(errno);
                set_failure("ROOT ENTRY STAT FAILED", result);
                break;
            }
            if (!S_ISDIR(status.st_mode) && !S_ISREG(status.st_mode)) {
                result = ESP_ERR_NOT_SUPPORTED;
                set_failure("SPECIAL ENTRY NOT SUPPORTED", result);
                break;
            }
            child_is_directory = S_ISDIR(status.st_mode);
        }

        if (closedir(root) != 0 && result == ESP_OK) {
            result = filesystem_error(errno);
            set_failure("CLOSE CARD ROOT FAILED", result);
        }
        if (result != ESP_OK) {
            std::free(child_path);
            break;
        }
        if (child_path == nullptr) {
            break;
        }

        ESP_LOGI(kTag, "Clear Card deleting: %s", child_path);
        if (child_is_directory) {
            result = delete_directory_tree(
                child_path, 0U,
                &clear_result.deleted_file_count,
                &clear_result.deleted_folder_count);
        } else if (unlink(child_path) == 0) {
            ++clear_result.deleted_file_count;
        } else {
            result = filesystem_error(errno);
            ESP_LOGE(kTag, "Clear Card file delete failed for %s: errno=%d (%s)",
                     child_path, errno, std::strerror(errno));
        }
        std::free(child_path);
        if (result != ESP_OK) {
            set_failure(child_is_directory ? "FOLDER DELETE FAILED"
                                           : "FILE DELETE FAILED",
                        result);
        }
    }

    const esp_err_t unmount_result =
        esp_vfs_fat_sdcard_unmount(kMountPoint, card);
    if (result == ESP_OK && unmount_result != ESP_OK) {
        result = unmount_result;
        set_failure("CARD UNMOUNT FAILED", result);
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (!card_is_inserted()) set_card_power(false);

    clear_result.passed = result == ESP_OK;
    clear_result.error = result;
    if (clear_result.passed) {
        std::snprintf(clear_result.failure_reason,
                      sizeof(clear_result.failure_reason), "NONE");
    }
    ESP_LOGI(kTag,
             "Clear Card complete: %s | files=%llu folders=%llu reason=%s error=%s",
             clear_result.passed ? "SUCCESS" : "FAILED",
             static_cast<unsigned long long>(
                 clear_result.deleted_file_count),
             static_cast<unsigned long long>(
                 clear_result.deleted_folder_count),
             clear_result.failure_reason,
             esp_err_to_name(clear_result.error));
    ESP_LOGI(kTag, "Clear Card finished; shared SPI2 is ready for display");
    return result;
}

esp_err_t sticky_sdcard_probe_initialize_target(
    StickySdInitializeResult &init_result)
{
    init_result = {};
    StickySdRawDiagnostics *diagnostics =
        static_cast<StickySdRawDiagnostics *>(heap_caps_calloc(
            1U, sizeof(StickySdRawDiagnostics), MALLOC_CAP_8BIT));
    if (diagnostics == nullptr) {
        init_result.error = ESP_ERR_NO_MEM;
        return init_result.error;
    }
    esp_err_t result = sticky_sdcard_get_raw_diagnostics(*diagnostics);
    if (result == ESP_OK &&
        !select_initialize_file_system(diagnostics->capacity_bytes,
                                       init_result)) {
        result = ESP_ERR_NOT_SUPPORTED;
    }
    init_result.error = result;
    ESP_LOGI(kTag, "Initialize target probe: capacity=%llu filesystem=%s (%s)",
             static_cast<unsigned long long>(
                 diagnostics->capacity_bytes),
             init_result.target_file_system, esp_err_to_name(result));
    heap_caps_free(diagnostics);
    return result;
}

esp_err_t sticky_sdcard_initialize_card(StickySdInitializeResult &init_result)
{
    constexpr size_t kWorkBufferSize = 4096U;
    constexpr DWORD kFat32ClusterSize = 32U * 1024U;
    static_assert(FF_VOLUMES > kInitializeFatFsDrive,
                  "Initialize Card requires FatFs logical drive 1");
    init_result = {};
    init_result.attempted = true;

    auto set_failure = [&init_result](const char *stage, esp_err_t error) {
        if (std::strcmp(init_result.failure_stage, "NOT STARTED") == 0 ||
            init_result.failure_stage[0] == '\0') {
            std::snprintf(init_result.failure_stage,
                          sizeof(init_result.failure_stage), "%s", stage);
        }
        init_result.error = error;
    };

    ESP_LOGW(kTag,
             "Initialize Card started: all existing card data will be erased");
    if (!s_initialized) {
        set_failure("SD INTERFACE NOT READY", ESP_ERR_INVALID_STATE);
        return init_result.error;
    }
    if (!card_is_inserted()) {
        set_card_power(false);
        set_failure("NO SD CARD", ESP_ERR_NOT_FOUND);
        return init_result.error;
    }

    esp_err_t result = gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (result == ESP_OK) result = set_card_power(true);
    if (result != ESP_OK) {
        set_failure("CARD POWER FAILED", result);
        return result;
    }
    vTaskDelay(kPowerOnDelay);

    sdspi_dev_handle_t device = -1;
    bool device_added = false;
    bool disk_registered = false;
    const BYTE drive = kInitializeFatFsDrive;
    uint32_t expected_partition_sectors = 0;
    void *work_buffer = nullptr;
    sdmmc_card_t *card = static_cast<sdmmc_card_t *>(
        heap_caps_calloc(1U, sizeof(sdmmc_card_t), MALLOC_CAP_8BIT));

    if (card == nullptr) {
        result = ESP_ERR_NO_MEM;
        set_failure("CARD STATE ALLOCATION FAILED", result);
    }

    if (result == ESP_OK) result = sdspi_host_init();
    if (result != ESP_OK) set_failure("SDSPI INIT FAILED", result);
    if (result == ESP_OK) {
        sdspi_device_config_t device_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        device_config.host_id = SPI2_HOST;
        device_config.gpio_cs = static_cast<gpio_num_t>(PIN_SD_CS);
        device_config.gpio_cd = GPIO_NUM_NC;
        device_config.gpio_wp = GPIO_NUM_NC;
        result = sdspi_host_init_device(&device_config, &device);
        device_added = result == ESP_OK;
        if (result != ESP_OK) set_failure("SD DEVICE INIT FAILED", result);
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    if (result == ESP_OK) {
        host.slot = device;
        result = sdmmc_card_init(&host, card);
        if (result != ESP_OK) set_failure("CARD INIT FAILED", result);
    }
    if (result == ESP_OK && card->csd.sector_size != 512U) {
        result = ESP_ERR_INVALID_SIZE;
        set_failure("UNSUPPORTED SECTOR SIZE", result);
    }
    if (result == ESP_OK && !select_initialize_file_system(
            static_cast<uint64_t>(card->csd.capacity) *
                card->csd.sector_size,
            init_result)) {
        result = ESP_ERR_NOT_SUPPORTED;
        set_failure("UNSUPPORTED CARD SIZE", result);
    }
    if (result == ESP_OK) {
        const uint8_t partition_type =
            init_result.target_exfat ? 0x07U : 0x0CU;
        result = write_initialize_mbr(card, partition_type,
                                      expected_partition_sectors);
        if (result != ESP_OK) {
            set_failure("MBR CREATE FAILED", result);
        } else {
            ESP_LOGI(kTag,
                     "Initialize Card MBR created: type=0x%02X start=%lu sectors=%lu",
                     static_cast<unsigned>(partition_type),
                     static_cast<unsigned long>(
                         kInitializePartitionStartLba),
                     static_cast<unsigned long>(expected_partition_sectors));
        }
    }
    if (result == ESP_OK) {
        ff_diskio_register_sdmmc(drive, card);
        disk_registered = true;
        work_buffer = heap_caps_malloc(kWorkBufferSize, MALLOC_CAP_8BIT);
        if (work_buffer == nullptr) {
            result = ESP_ERR_NO_MEM;
            set_failure("WORK BUFFER ALLOCATION FAILED", result);
        }
    }
    if (result == ESP_OK) {
        char drive_path[3] = {
            static_cast<char>('0' + drive), ':', '\0',
        };
        const MKFS_PARM options = {
            .fmt = static_cast<BYTE>(
                init_result.target_exfat ? FM_EXFAT : FM_FAT32),
            .n_fat = static_cast<BYTE>(
                init_result.target_exfat ? 1U : 2U),
            .align = 0,
            .n_root = 0,
            .au_size = init_result.target_exfat ? 0U : kFat32ClusterSize,
        };
        ESP_LOGI(kTag,
                 "Initialize Card formatting existing MBR partition 1 on drive %s as %s, allocation unit=%s",
                 drive_path, init_result.target_file_system,
                 init_result.target_exfat ? "AUTO" : "32 KiB");
        const FRESULT format_result = f_mkfs(
            drive_path, &options, work_buffer,
            static_cast<UINT>(kWorkBufferSize));
        if (format_result != FR_OK) {
            result = ESP_FAIL;
            set_failure(init_result.target_exfat ? "exFAT FORMAT FAILED"
                                                 : "FAT32 FORMAT FAILED",
                        result);
            ESP_LOGE(kTag, "f_mkfs failed: FRESULT=%u",
                     static_cast<unsigned>(format_result));
        } else {
            init_result.format_pass = true;
            ESP_LOGI(kTag, "Initialize Card format: PASS");
        }
    }

    if (work_buffer != nullptr) heap_caps_free(work_buffer);
    if (disk_registered) ff_diskio_unregister(drive);
    if (device_added) {
        const esp_err_t remove_result = sdspi_host_remove_device(device);
        if (result == ESP_OK && remove_result != ESP_OK) {
            result = remove_result;
            set_failure("SDSPI RELEASE FAILED", result);
        }
    }
    if (card != nullptr) {
        heap_caps_free(card);
        card = nullptr;
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (!card_is_inserted()) {
        set_card_power(false);
        if (result == ESP_OK) {
            result = ESP_ERR_NOT_FOUND;
            set_failure("CARD REMOVED", result);
        }
    }

    if (result == ESP_OK) {
        StickySdRawDiagnostics *diagnostics =
            static_cast<StickySdRawDiagnostics *>(heap_caps_calloc(
                1U, sizeof(StickySdRawDiagnostics), MALLOC_CAP_8BIT));
        if (diagnostics == nullptr) {
            result = ESP_ERR_NO_MEM;
            set_failure("RAW VERIFY ALLOCATION FAILED", result);
        } else {
            result = sticky_sdcard_get_raw_diagnostics(*diagnostics);
        }
        bool has_expected_partition = false;
        bool start_lba_matches = false;
        bool partition_size_matches = false;
        bool exfat_partition_offset_matches = !init_result.target_exfat;
        bool exfat_volume_length_valid = !init_result.target_exfat;
        uint64_t verified_partition_offset = 0;
        uint64_t verified_volume_length = 0;
        const uint8_t expected_partition_type =
            init_result.target_exfat ? 0x07U : 0x0CU;
        if (diagnostics != nullptr) {
            for (size_t index = 0;
                 index < diagnostics->detected_partition_count; ++index) {
                const StickySdDetectedPartition &partition =
                    diagnostics->detected_partitions[index];
                if (!partition.from_gpt && partition.range_valid &&
                    partition.mbr_type == expected_partition_type) {
                    has_expected_partition = true;
                    start_lba_matches =
                        partition.start_lba == kInitializePartitionStartLba;
                    partition_size_matches =
                        partition.sector_count == expected_partition_sectors;
                    if (init_result.target_exfat) {
                        verified_partition_offset =
                            partition.exfat_partition_offset;
                        verified_volume_length =
                            partition.exfat_volume_length;
                        exfat_partition_offset_matches =
                            verified_partition_offset ==
                            kInitializePartitionStartLba;
                        exfat_volume_length_valid =
                            verified_volume_length != 0U &&
                            verified_volume_length <= partition.sector_count;
                    }
                    break;
                }
            }
        }
        init_result.mbr_pass = result == ESP_OK &&
            diagnostics != nullptr && diagnostics->mbr_signature_valid &&
            diagnostics->scheme == StickySdPartitionScheme::Mbr &&
            has_expected_partition && start_lba_matches &&
            partition_size_matches;
        if (!init_result.mbr_pass) {
            if (result == ESP_OK) result = ESP_FAIL;
            set_failure("MBR VERIFY FAILED", result);
        } else {
            init_result.file_system_pass = std::strcmp(
                diagnostics->detected_file_system,
                init_result.target_file_system) == 0 &&
                exfat_partition_offset_matches &&
                exfat_volume_length_valid;
            if (!init_result.file_system_pass) {
                result = ESP_FAIL;
                if (init_result.target_exfat &&
                    (!exfat_partition_offset_matches ||
                     !exfat_volume_length_valid)) {
                    set_failure("exFAT GEOMETRY VERIFY FAILED", result);
                } else {
                    set_failure(init_result.target_exfat
                                    ? "exFAT VERIFY FAILED"
                                    : "FAT32 VERIFY FAILED",
                                result);
                }
            }
        }
        if (init_result.target_exfat) {
            ESP_LOGI(kTag,
                     "Initialize Card raw verify: MBR=%s start=%s size=%s exFAT=%s partition_offset=%llu volume_length=%llu",
                     init_result.mbr_pass ? "PASS" : "FAIL",
                     start_lba_matches ? "PASS" : "FAIL",
                     partition_size_matches ? "PASS" : "FAIL",
                     init_result.file_system_pass ? "PASS" : "FAIL",
                     static_cast<unsigned long long>(
                         verified_partition_offset),
                     static_cast<unsigned long long>(verified_volume_length));
        } else {
            ESP_LOGI(kTag,
                     "Initialize Card raw verify: MBR=%s start=%s size=%s FAT32=%s",
                     init_result.mbr_pass ? "PASS" : "FAIL",
                     start_lba_matches ? "PASS" : "FAIL",
                     partition_size_matches ? "PASS" : "FAIL",
                     init_result.file_system_pass ? "PASS" : "FAIL");
        }
        if (diagnostics != nullptr) {
            heap_caps_free(diagnostics);
            diagnostics = nullptr;
        }
    }

    if (result == ESP_OK) {
        StickySdCardInfo *info = static_cast<StickySdCardInfo *>(
            heap_caps_calloc(1U, sizeof(StickySdCardInfo), MALLOC_CAP_8BIT));
        if (info == nullptr) {
            result = ESP_ERR_NO_MEM;
            set_failure("MOUNT VERIFY ALLOCATION FAILED", result);
        } else {
            result = sticky_sdcard_get_info(*info);
        }
        init_result.mount_pass = result == ESP_OK && info != nullptr &&
            info->mounted && std::strcmp(info->partition, "MBR") == 0 &&
            std::strcmp(info->file_system,
                        init_result.target_file_system) == 0 &&
            (init_result.target_exfat ||
             info->cluster_size == kFat32ClusterSize);
        if (!init_result.mount_pass) {
            if (result == ESP_OK) result = ESP_FAIL;
            set_failure("MOUNT VERIFY FAILED", result);
        }
        ESP_LOGI(kTag,
                 "Initialize Card mount verify: %s (partition=%s filesystem=%s cluster=%lu)",
                 init_result.mount_pass ? "PASS" : "FAIL",
                 info != nullptr ? info->partition : "--",
                 info != nullptr ? info->file_system : "--",
                 static_cast<unsigned long>(
                     info != nullptr ? info->cluster_size : 0U));
        if (info != nullptr) {
            heap_caps_free(info);
            info = nullptr;
        }
    }

    init_result.passed = result == ESP_OK && init_result.format_pass &&
        init_result.mbr_pass && init_result.file_system_pass &&
        init_result.mount_pass;
    init_result.error = result;
    if (init_result.passed) {
        std::snprintf(init_result.failure_stage,
                      sizeof(init_result.failure_stage), "NONE");
    }
    ESP_LOGI(kTag,
             "Initialize Card complete: %s | target=%s format=%s MBR=%s filesystem=%s mount=%s stage=%s error=%s",
             init_result.passed ? "CARD READY TO USE" : "FAILED",
             init_result.target_file_system,
             init_result.format_pass ? "PASS" : "FAIL",
             init_result.mbr_pass ? "PASS" : "FAIL",
             init_result.file_system_pass ? "PASS" : "FAIL",
             init_result.mount_pass ? "PASS" : "FAIL",
             init_result.failure_stage, esp_err_to_name(init_result.error));
    ESP_LOGI(kTag,
             "Initialize Card finished; shared SPI2 is ready for display");
    return result;
}

esp_err_t sticky_sdcard_list_directory(const char *directory_path,
                                       StickySdDirectoryEntry *entries,
                                       size_t entry_capacity,
                                       size_t entry_offset,
                                       size_t &entry_count,
                                       size_t &total_entry_count)
{
    entry_count = 0;
    total_entry_count = 0;
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (directory_path == nullptr || entries == nullptr ||
        entry_capacity == 0 || directory_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!card_is_inserted()) {
        set_card_power(false);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t result = gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (result == ESP_OK) {
        result = set_card_power(true);
    }
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(kPowerOnDelay);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = static_cast<gpio_num_t>(PIN_SD_CS);
    slot_config.host_id = SPI2_HOST;
    slot_config.gpio_cd = GPIO_NUM_NC;
    slot_config.gpio_wp = GPIO_NUM_NC;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = nullptr;
    result = esp_vfs_fat_sdspi_mount(
        kMountPoint, &host, &slot_config, &mount_config, &card);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "MicroSD mount failed for directory listing: %s",
                 esp_err_to_name(result));
        gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
        if (!card_is_inserted()) {
            set_card_power(false);
        }
        return result;
    }

    char full_directory_path[384] = {};
    const int directory_length = std::snprintf(
        full_directory_path,
        sizeof(full_directory_path),
        "%s%s",
        kMountPoint,
        directory_path);
    if (directory_length <= 0 ||
        static_cast<size_t>(directory_length) >= sizeof(full_directory_path)) {
        result = ESP_ERR_INVALID_SIZE;
    } else {
        DIR *directory = opendir(full_directory_path);
        if (directory == nullptr) {
            ESP_LOGE(kTag, "Open directory %s failed: errno=%d (%s)",
                     full_directory_path, errno, std::strerror(errno));
            result = ESP_ERR_NOT_FOUND;
        } else {
            while (dirent *item = readdir(directory)) {
                if (std::strcmp(item->d_name, ".") == 0 ||
                    std::strcmp(item->d_name, "..") == 0) {
                    continue;
                }
                char item_path[512] = {};
                const int item_length = std::snprintf(
                    item_path,
                    sizeof(item_path),
                    "%s/%s",
                    full_directory_path,
                    item->d_name);
                if (item_length <= 0 ||
                    static_cast<size_t>(item_length) >= sizeof(item_path)) {
                    continue;
                }

                struct stat status = {};
                if (stat(item_path, &status) != 0) {
                    ESP_LOGW(kTag, "Stat %s failed: errno=%d (%s)",
                             item_path, errno, std::strerror(errno));
                    continue;
                }

                const size_t directory_index = total_entry_count++;
                if (directory_index < entry_offset ||
                    entry_count >= entry_capacity) {
                    continue;
                }

                StickySdDirectoryEntry &entry = entries[entry_count++];
                std::snprintf(entry.name, sizeof(entry.name), "%s", item->d_name);
                entry.is_directory = S_ISDIR(status.st_mode);
                entry.size = entry.is_directory
                                 ? 0
                                 : static_cast<uint64_t>(status.st_size);
                entry.modified_time = static_cast<int64_t>(status.st_mtime);
            }
            closedir(directory);
            std::sort(entries,
                      entries + entry_count,
                      [](const StickySdDirectoryEntry &left,
                         const StickySdDirectoryEntry &right) {
                          if (left.is_directory != right.is_directory) {
                              return left.is_directory;
                          }
                          return std::strcmp(left.name, right.name) < 0;
                      });
        }
    }

    const esp_err_t unmount_result =
        esp_vfs_fat_sdcard_unmount(kMountPoint, card);
    if (result == ESP_OK && unmount_result != ESP_OK) {
        result = unmount_result;
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (!card_is_inserted()) {
        set_card_power(false);
    }
    ESP_LOGI(kTag, "Directory query complete; shared SPI2 is ready for display");
    return result;
}

esp_err_t sticky_sdcard_get_directory_stats(
    const char *directory_path,
    StickySdDirectoryStats &stats)
{
    stats = {};
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!valid_directory_path(directory_path) ||
        std::strcmp(directory_path, "/") == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!card_is_inserted()) {
        set_card_power(false);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t result = gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (result == ESP_OK) {
        result = set_card_power(true);
    }
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(kPowerOnDelay);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = static_cast<gpio_num_t>(PIN_SD_CS);
    slot_config.host_id = SPI2_HOST;
    slot_config.gpio_cd = GPIO_NUM_NC;
    slot_config.gpio_wp = GPIO_NUM_NC;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = nullptr;
    result = esp_vfs_fat_sdspi_mount(
        kMountPoint, &host, &slot_config, &mount_config, &card);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "MicroSD mount failed for folder stats: %s",
                 esp_err_to_name(result));
        gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
        if (!card_is_inserted()) {
            set_card_power(false);
        }
        return result;
    }

    char target_path[kMaximumSdPathSize] = {};
    const int target_length = std::snprintf(
        target_path, sizeof(target_path), "%s%s", kMountPoint, directory_path);
    if (target_length <= 0 ||
        static_cast<size_t>(target_length) >= sizeof(target_path)) {
        result = ESP_ERR_INVALID_SIZE;
    } else {
        result = scan_directory_tree(target_path, 0U, stats);
    }

    const esp_err_t unmount_result =
        esp_vfs_fat_sdcard_unmount(kMountPoint, card);
    if (result == ESP_OK && unmount_result != ESP_OK) {
        result = unmount_result;
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (!card_is_inserted()) {
        set_card_power(false);
    }
    if (result != ESP_OK) {
        stats = {};
    }
    ESP_LOGI(kTag, "Folder stats complete; shared SPI2 is ready for display");
    return result;
}

esp_err_t sticky_sdcard_delete_file(const char *directory_path,
                                    const char *file_name)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (directory_path == nullptr || directory_path[0] != '/' ||
        !valid_entry_name(file_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!card_is_inserted()) {
        set_card_power(false);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t result = gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (result == ESP_OK) {
        result = set_card_power(true);
    }
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(kPowerOnDelay);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = static_cast<gpio_num_t>(PIN_SD_CS);
    slot_config.host_id = SPI2_HOST;
    slot_config.gpio_cd = GPIO_NUM_NC;
    slot_config.gpio_wp = GPIO_NUM_NC;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = nullptr;
    result = esp_vfs_fat_sdspi_mount(
        kMountPoint, &host, &slot_config, &mount_config, &card);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "MicroSD mount failed for file deletion: %s",
                 esp_err_to_name(result));
        gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
        if (!card_is_inserted()) {
            set_card_power(false);
        }
        return result;
    }

    char file_path[640] = {};
    result = build_entry_path(
        directory_path, file_name, file_path, sizeof(file_path));
    if (result == ESP_OK) {
        struct stat status = {};
        if (stat(file_path, &status) != 0) {
            ESP_LOGE(kTag, "Stat before delete failed for %s: errno=%d (%s)",
                     file_path, errno, std::strerror(errno));
            result = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        } else if (!S_ISREG(status.st_mode)) {
            ESP_LOGW(kTag, "Delete rejected for non-file path: %s", file_path);
            result = ESP_ERR_INVALID_ARG;
        } else if (unlink(file_path) != 0) {
            ESP_LOGE(kTag, "Delete %s failed: errno=%d (%s)",
                     file_path, errno, std::strerror(errno));
            result = errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
        } else {
            ESP_LOGI(kTag, "Deleted file: %s", file_path);
            result = ESP_OK;
        }
    }

    const esp_err_t unmount_result =
        esp_vfs_fat_sdcard_unmount(kMountPoint, card);
    if (result == ESP_OK && unmount_result != ESP_OK) {
        result = unmount_result;
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (!card_is_inserted()) {
        set_card_power(false);
    }
    ESP_LOGI(kTag, "File delete operation complete; shared SPI2 is ready");
    return result;
}

esp_err_t sticky_sdcard_delete_directory(const char *directory_path,
                                         const char *directory_name)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!valid_directory_path(directory_path) ||
        !valid_entry_name(directory_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!card_is_inserted()) {
        set_card_power(false);
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t result = gpio_set_level(
        static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (result == ESP_OK) {
        result = set_card_power(true);
    }
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(kPowerOnDelay);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = static_cast<gpio_num_t>(PIN_SD_CS);
    slot_config.host_id = SPI2_HOST;
    slot_config.gpio_cd = GPIO_NUM_NC;
    slot_config.gpio_wp = GPIO_NUM_NC;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = nullptr;
    result = esp_vfs_fat_sdspi_mount(
        kMountPoint, &host, &slot_config, &mount_config, &card);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "MicroSD mount failed for folder delete: %s",
                 esp_err_to_name(result));
        gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
        if (!card_is_inserted()) {
            set_card_power(false);
        }
        return result;
    }

    char target_path[kMaximumSdPathSize] = {};
    result = build_entry_path(directory_path,
                              directory_name,
                              target_path,
                              sizeof(target_path));
    if (result == ESP_OK) {
        // build_entry_path always appends one validated component, so the
        // mounted root itself can never become the recursive-delete target.
        result = delete_directory_tree(target_path, 0U);
        if (result == ESP_OK) {
            ESP_LOGI(kTag, "Deleted directory tree: %s", target_path);
        }
    }

    const esp_err_t unmount_result =
        esp_vfs_fat_sdcard_unmount(kMountPoint, card);
    if (result == ESP_OK && unmount_result != ESP_OK) {
        result = unmount_result;
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (!card_is_inserted()) {
        set_card_power(false);
    }
    ESP_LOGI(kTag,
             "Folder delete operation complete; shared SPI2 is ready");
    return result;
}

esp_err_t sticky_sdcard_create_entry(const char *directory_path,
                                     const char *entry_name,
                                     bool create_directory)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (directory_path == nullptr || directory_path[0] != '/' ||
        !valid_entry_name(entry_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!card_is_inserted()) {
        set_card_power(false);
        return ESP_ERR_NOT_FOUND;
    }

    // ESP-IDF FatFs derives directory-entry timestamps from the system clock.
    // A bad or unpowered RTC must not make an otherwise valid create fail.
    const esp_err_t clock_result = sticky_rtc_sync_system_time();
    if (clock_result != ESP_OK) {
        ESP_LOGW(kTag, "RTC sync failed; using current system time: %s",
                 esp_err_to_name(clock_result));
    }
    const time_t entry_time = time(nullptr);
    if (!valid_fat_time(entry_time)) {
        ESP_LOGW(kTag, "System time is outside the FAT timestamp range");
    }

    esp_err_t result = gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (result == ESP_OK) {
        result = set_card_power(true);
    }
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(kPowerOnDelay);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = static_cast<gpio_num_t>(PIN_SD_CS);
    slot_config.host_id = SPI2_HOST;
    slot_config.gpio_cd = GPIO_NUM_NC;
    slot_config.gpio_wp = GPIO_NUM_NC;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 4;
    mount_config.allocation_unit_size = 16 * 1024;

    sdmmc_card_t *card = nullptr;
    result = esp_vfs_fat_sdspi_mount(
        kMountPoint, &host, &slot_config, &mount_config, &card);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "MicroSD mount failed for create operation: %s",
                 esp_err_to_name(result));
        gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
        if (!card_is_inserted()) {
            set_card_power(false);
        }
        return result;
    }

    char entry_path[640] = {};
    result = build_entry_path(
        directory_path, entry_name, entry_path, sizeof(entry_path));
    if (result == ESP_OK) {
        struct stat status = {};
        if (stat(entry_path, &status) == 0) {
            result = ESP_ERR_INVALID_STATE;
        } else if (errno != ENOENT) {
            ESP_LOGE(kTag, "Existence check failed for %s: errno=%d (%s)",
                     entry_path, errno, std::strerror(errno));
            result = ESP_FAIL;
        } else if (create_directory) {
            if (mkdir(entry_path, 0777) != 0) {
                result = errno == EEXIST ? ESP_ERR_INVALID_STATE : ESP_FAIL;
                ESP_LOGE(kTag, "Create directory %s failed: errno=%d (%s)",
                         entry_path, errno, std::strerror(errno));
            }
        } else {
            const int descriptor = open(
                entry_path, O_WRONLY | O_CREAT | O_EXCL, 0666);
            if (descriptor < 0) {
                result = errno == EEXIST ? ESP_ERR_INVALID_STATE : ESP_FAIL;
                ESP_LOGE(kTag, "Create file %s failed: errno=%d (%s)",
                         entry_path, errno, std::strerror(errno));
            } else if (close(descriptor) != 0) {
                result = ESP_FAIL;
                ESP_LOGE(kTag, "Close new file %s failed: errno=%d (%s)",
                         entry_path, errno, std::strerror(errno));
            }
        }
        if (result == ESP_OK) {
            // Force the modification time through the VFS f_utime path, then
            // read it back before unmounting. This makes timestamp failures
            // visible and does not rely solely on the implicit create time.
            apply_and_verify_entry_time(entry_path, entry_time);
            ESP_LOGI(kTag, "Created %s: %s",
                     create_directory ? "directory" : "empty file",
                     entry_path);
        }
    }

    const esp_err_t unmount_result =
        esp_vfs_fat_sdcard_unmount(kMountPoint, card);
    if (result == ESP_OK && unmount_result != ESP_OK) {
        result = unmount_result;
    }
    gpio_set_level(static_cast<gpio_num_t>(PIN_SD_CS), 1);
    if (!card_is_inserted()) {
        set_card_power(false);
    }
    ESP_LOGI(kTag, "Create operation complete; shared SPI2 is ready");
    return result;
}
