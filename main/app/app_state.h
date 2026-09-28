#pragma once

#include <cstdint>

#include "sticky_rtc.h"
#include "sticky_sdcard.h"
#include "capacity_format.h"

enum class PageId {
    SdInfo,
    Files,
    FileDetails,
    DeleteConfirm,
    NewEntryMenu,
    SetDateTime,
    SetDateTimeNumber,
    NameInput,
    Help,
    Tools,
    Diagnostics,
    StorageTest,
    ClearCard,
    ClearCardConfirm,
    InitializeCard,
    InitializeCardConfirm,
};

enum class CreateEntryType {
    File,
    Folder,
};

enum class CreateFileExtension : uint8_t {
    Txt,
    Json,
    Csv,
    Md,
    Bin,
};

enum class HelpSection : uint8_t {
    About,
    Guide,
};

constexpr size_t kFileBrowserPathSize = 256;
constexpr size_t kFileBrowserEntriesPerPage = 6;

struct FileBrowserState {
    char path[kFileBrowserPathSize] = "/";
    StickySdDirectoryEntry entries[kFileBrowserEntriesPerPage] = {};
    size_t entry_count = 0;
    size_t selected = 0;
    size_t total_entry_count = 0;
    size_t page_index = 0;
    size_t page_count = 1;
    bool valid = false;
    esp_err_t error = ESP_ERR_INVALID_STATE;
};

struct FileActionState {
    char name[kStickySdFileNameSize] = {};
    char path[kFileBrowserPathSize] = {};
    uint64_t size = 0;
    int64_t modified_time = 0;
    uint64_t contained_file_count = 0;
    uint64_t contained_folder_count = 0;
    bool is_directory = false;
    bool directory_not_empty = false;
    bool directory_stats_valid = false;
    esp_err_t error = ESP_OK;
};

constexpr size_t kCreateEntryNameSize = kStickySdFileNameSize;

struct CreateEntryState {
    CreateEntryType type = CreateEntryType::Folder;
    char name[kCreateEntryNameSize] = {};
    size_t length = 0;
    bool uppercase = false;
    CreateFileExtension extension = CreateFileExtension::Txt;
    esp_err_t error = ESP_OK;
};

enum class DateTimeField : uint8_t {
    Year,
    Month,
    Day,
    Hour,
    Minute,
};

struct RtcSetupState {
    RtcDateTime value = {2026, 9, 20, 12, 30, 0};
    CreateEntryType pending_type = CreateEntryType::Folder;
    esp_err_t error = ESP_OK;
};

struct DateTimeNumberInputState {
    DateTimeField field = DateTimeField::Year;
    char digits[5] = {};
    size_t length = 0;
    esp_err_t error = ESP_OK;
};

struct BatteryStatusState {
    int percent = -1;
    uint16_t voltage_mv = 0;
    bool valid = false;
    bool charging = false;
    bool charging_valid = false;
};

struct AppState {
    PageId current_page = PageId::SdInfo;
    StickySdCardInfo sd_card;
    FileBrowserState files;
    FileActionState file_action;
    CreateEntryState create_entry;
    RtcSetupState rtc_setup;
    DateTimeNumberInputState date_time_input;
    BatteryStatusState battery;
    HelpSection help_section = HelpSection::About;
    CapacityUnitMode capacity_unit_mode = CapacityUnitMode::Decimal;
    StickySdRawDiagnostics diagnostics;
    size_t diagnostics_page = 0;
    StickySdStorageTestResult storage_test;
    StickySdClearCardResult clear_card;
    StickySdInitializeResult initialize_card;
    bool initialize_running = false;
};
