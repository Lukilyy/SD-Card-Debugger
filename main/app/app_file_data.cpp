#include "app_data.h"
#include "app_file_data.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app_state.h"
#include "esp_log.h"
#include "sticky_rtc.h"
#include "sticky_sdcard.h"

namespace {

constexpr char kTag[] = "app_data";

constexpr const char *kFileExtensions[] = {
    "txt", "json", "csv", "md", "bin",
};

int days_in_month(int year, int month)
{
    constexpr int kDays[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    int days = kDays[month - 1];
    const bool leap =
        (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap) {
        days = 29;
    }
    return days;
}

int wrap_value(int value, int minimum, int maximum, int direction)
{
    value += direction;
    if (value < minimum) {
        return maximum;
    }
    if (value > maximum) {
        return minimum;
    }
    return value;
}

void open_name_input(AppState &state, CreateEntryType type)
{
    state.create_entry = {};
    state.create_entry.type = type;
    state.current_page = PageId::NameInput;
}

esp_err_t build_browser_child_path(const char *parent,
                                   const char *name,
                                   char *output,
                                   size_t output_size)
{
    if (parent == nullptr || name == nullptr || output == nullptr ||
        output_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    const int length = std::strcmp(parent, "/") == 0
                           ? std::snprintf(output, output_size, "/%s", name)
                           : std::snprintf(output, output_size,
                                           "%s/%s", parent, name);
    return length > 0 && static_cast<size_t>(length) < output_size
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

}  // namespace

void handle_sd_card_change(AppState &state)
{
    update_sd_card_info(state);
    if (state.sd_card.inserted && state.current_page == PageId::Files) {
        std::snprintf(state.files.path, sizeof(state.files.path), "/");
        state.files.page_index = 0;
        reload_file_browser(state);
    } else if (!state.sd_card.inserted) {
        state.files.entry_count = 0;
        state.files.total_entry_count = 0;
        state.files.selected = 0;
        state.files.page_index = 0;
        state.files.page_count = 1;
        state.files.valid = false;
        state.files.error = ESP_ERR_NOT_FOUND;
        if (state.current_page == PageId::FileDetails ||
            state.current_page == PageId::DeleteConfirm ||
            state.current_page == PageId::NewEntryMenu ||
            state.current_page == PageId::SetDateTime ||
            state.current_page == PageId::SetDateTimeNumber ||
            state.current_page == PageId::NameInput) {
            state.current_page = PageId::Files;
        }
    }
}

void reload_file_browser(AppState &state)
{
    FileBrowserState &files = state.files;
    files.entry_count = 0;
    files.selected = 0;
    files.total_entry_count = 0;
    files.error = sticky_sdcard_list_directory(
        files.path,
        files.entries,
        kFileBrowserEntriesPerPage,
        files.page_index * kFileBrowserEntriesPerPage,
        files.entry_count,
        files.total_entry_count);
    files.valid = files.error == ESP_OK;
    files.page_count = files.valid
                           ? std::max<size_t>(
                                 1U,
                                 (files.total_entry_count +
                                  kFileBrowserEntriesPerPage - 1U) /
                                     kFileBrowserEntriesPerPage)
                           : 1U;
    if (files.valid && files.page_index >= files.page_count) {
        files.page_index = files.page_count - 1U;
        reload_file_browser(state);
        return;
    }
    if (!files.valid) {
        ESP_LOGW(kTag, "Directory load failed for %s: %s",
                 files.path, esp_err_to_name(files.error));
    }
}

void open_file_browser(AppState &state)
{
    state.current_page = PageId::Files;
    std::snprintf(state.files.path, sizeof(state.files.path), "/");
    state.files.page_index = 0;
    reload_file_browser(state);
}

void select_previous_file_entry(AppState &state)
{
    const size_t count = state.files.entry_count;
    if (count == 0) {
        return;
    }
    state.files.selected = state.files.selected == 0
                               ? count - 1U
                               : state.files.selected - 1U;
}

void select_next_file_entry(AppState &state)
{
    const size_t count = state.files.entry_count;
    if (count == 0) {
        return;
    }
    state.files.selected = (state.files.selected + 1U) % count;
}

void file_browser_go_parent(AppState &state)
{
    FileBrowserState &files = state.files;
    if (std::strcmp(files.path, "/") == 0) {
        state.current_page = PageId::SdInfo;
        return;
    }

    char *separator = std::strrchr(files.path, '/');
    if (separator == nullptr || separator == files.path) {
        std::snprintf(files.path, sizeof(files.path), "/");
    } else {
        *separator = '\0';
    }
    files.page_index = 0;
    reload_file_browser(state);
}

void activate_selected_file_entry(AppState &state)
{
    FileBrowserState &files = state.files;
    if (!files.valid) {
        reload_file_browser(state);
        return;
    }

    const size_t index = files.selected;
    if (index >= files.entry_count) {
        return;
    }

    if (!files.entries[index].is_directory) {
        state.file_action = {};
        std::snprintf(state.file_action.name,
                      sizeof(state.file_action.name),
                      "%s",
                      files.entries[index].name);
        state.file_action.size = files.entries[index].size;
        state.file_action.modified_time = files.entries[index].modified_time;
        state.file_action.is_directory = false;
        state.file_action.error = build_browser_child_path(
            files.path, files.entries[index].name,
            state.file_action.path, sizeof(state.file_action.path));
        state.current_page = PageId::FileDetails;
        return;
    }

    char child_path[kFileBrowserPathSize] = {};
    const esp_err_t path_result = build_browser_child_path(
        files.path, files.entries[index].name,
        child_path, sizeof(child_path));
    if (path_result != ESP_OK) {
        files.error = path_result;
        files.valid = false;
        return;
    }

    std::snprintf(files.path, sizeof(files.path), "%s", child_path);
    files.page_index = 0;
    reload_file_browser(state);
}

void open_selected_folder_details(AppState &state)
{
    FileBrowserState &files = state.files;
    const size_t index = files.selected;
    if (!files.valid || index >= files.entry_count ||
        !files.entries[index].is_directory) {
        return;
    }

    state.file_action = {};
    std::snprintf(state.file_action.name,
                  sizeof(state.file_action.name),
                  "%s",
                  files.entries[index].name);
    state.file_action.modified_time = files.entries[index].modified_time;
    state.file_action.is_directory = true;

    state.file_action.error = build_browser_child_path(
        files.path, files.entries[index].name,
        state.file_action.path, sizeof(state.file_action.path));
    if (state.file_action.error == ESP_OK) {
        StickySdDirectoryStats stats = {};
        state.file_action.error = sticky_sdcard_get_directory_stats(
            state.file_action.path, stats);
        if (state.file_action.error == ESP_OK) {
            state.file_action.size = stats.total_file_bytes;
            state.file_action.contained_file_count = stats.file_count;
            state.file_action.contained_folder_count = stats.folder_count;
            state.file_action.directory_not_empty =
                stats.file_count > 0U || stats.folder_count > 0U;
            state.file_action.directory_stats_valid = true;
        }
    }
    state.current_page = PageId::FileDetails;
}

void file_browser_previous_page(AppState &state)
{
    if (!state.files.valid || state.files.page_index == 0) {
        return;
    }
    --state.files.page_index;
    reload_file_browser(state);
}

void file_browser_next_page(AppState &state)
{
    if (!state.files.valid ||
        state.files.page_index + 1U >= state.files.page_count) {
        return;
    }
    ++state.files.page_index;
    reload_file_browser(state);
}

esp_err_t delete_selected_file(AppState &state)
{
    const esp_err_t result = sticky_sdcard_delete_file(
        state.files.path, state.file_action.name);
    state.file_action.error = result;
    if (result != ESP_OK) {
        state.current_page = PageId::FileDetails;
        return result;
    }

    reload_file_browser(state);
    update_sd_card_info(state);
    state.current_page = PageId::Files;
    return ESP_OK;
}

esp_err_t delete_selected_folder(AppState &state)
{
    const esp_err_t result = sticky_sdcard_delete_directory(
        state.files.path, state.file_action.name);
    state.file_action.error = result;
    if (result != ESP_OK) {
        state.current_page = PageId::FileDetails;
        return result;
    }

    reload_file_browser(state);
    update_sd_card_info(state);
    state.current_page = PageId::Files;
    return ESP_OK;
}

void open_new_entry_menu(AppState &state)
{
    state.create_entry = {};
    state.current_page = PageId::NewEntryMenu;
}

void begin_create_entry(AppState &state, CreateEntryType type)
{
    const esp_err_t sync_result = sticky_rtc_sync_system_time();
    if (sync_result == ESP_OK) {
        open_name_input(state, type);
        return;
    }

    state.rtc_setup = {};
    state.rtc_setup.pending_type = type;
    state.rtc_setup.error = sync_result;
    RtcDateTime candidate = {};
    if (sticky_rtc_read_untrusted(candidate) == ESP_OK &&
        candidate.year >= 2020 && candidate.year <= 2099) {
        candidate.second = 0;
        state.rtc_setup.value = candidate;
    }
    state.current_page = PageId::SetDateTime;
}

void adjust_rtc_setup_value(AppState &state,
                            DateTimeField field,
                            int direction)
{
    if (direction != -1 && direction != 1) {
        return;
    }
    RtcDateTime &value = state.rtc_setup.value;
    switch (field) {
    case DateTimeField::Year:
        value.year = wrap_value(value.year, 2000, 2099, direction);
        break;
    case DateTimeField::Month:
        value.month = wrap_value(value.month, 1, 12, direction);
        break;
    case DateTimeField::Day:
        value.day = wrap_value(
            value.day, 1, days_in_month(value.year, value.month), direction);
        break;
    case DateTimeField::Hour:
        value.hour = wrap_value(value.hour, 0, 23, direction);
        break;
    case DateTimeField::Minute:
        value.minute = wrap_value(value.minute, 0, 59, direction);
        break;
    }
    const int maximum_day = days_in_month(value.year, value.month);
    if (value.day > maximum_day) {
        value.day = maximum_day;
    }
    value.second = 0;
    state.rtc_setup.error = ESP_OK;
}

void begin_date_time_number_input(AppState &state, DateTimeField field)
{
    state.date_time_input = {};
    state.date_time_input.field = field;
    state.current_page = PageId::SetDateTimeNumber;
}

void append_date_time_digit(AppState &state, char digit)
{
    if (digit < '0' || digit > '9') {
        return;
    }
    DateTimeNumberInputState &input = state.date_time_input;
    const size_t maximum_length =
        input.field == DateTimeField::Year ? 4U : 2U;
    if (input.length >= maximum_length) {
        input.error = ESP_ERR_INVALID_SIZE;
        return;
    }
    input.digits[input.length++] = digit;
    input.digits[input.length] = '\0';
    input.error = ESP_OK;
}

void delete_date_time_digit(AppState &state)
{
    DateTimeNumberInputState &input = state.date_time_input;
    if (input.length > 0U) {
        input.digits[--input.length] = '\0';
    }
    input.error = ESP_OK;
}

esp_err_t confirm_date_time_number_input(AppState &state)
{
    DateTimeNumberInputState &input = state.date_time_input;
    if (input.length == 0U) {
        input.error = ESP_ERR_INVALID_ARG;
        return input.error;
    }

    int entered = 0;
    for (size_t index = 0; index < input.length; ++index) {
        entered = entered * 10 + (input.digits[index] - '0');
    }

    RtcDateTime &value = state.rtc_setup.value;
    int minimum = 0;
    int maximum = 0;
    switch (input.field) {
    case DateTimeField::Year:
        minimum = 2000;
        maximum = 2099;
        break;
    case DateTimeField::Month:
        minimum = 1;
        maximum = 12;
        break;
    case DateTimeField::Day:
        minimum = 1;
        maximum = days_in_month(value.year, value.month);
        break;
    case DateTimeField::Hour:
        minimum = 0;
        maximum = 23;
        break;
    case DateTimeField::Minute:
        minimum = 0;
        maximum = 59;
        break;
    }
    if (entered < minimum || entered > maximum) {
        input.error = ESP_ERR_INVALID_ARG;
        return input.error;
    }

    switch (input.field) {
    case DateTimeField::Year:
        value.year = entered;
        break;
    case DateTimeField::Month:
        value.month = entered;
        break;
    case DateTimeField::Day:
        value.day = entered;
        break;
    case DateTimeField::Hour:
        value.hour = entered;
        break;
    case DateTimeField::Minute:
        value.minute = entered;
        break;
    }
    const int maximum_day = days_in_month(value.year, value.month);
    if (value.day > maximum_day) {
        value.day = maximum_day;
    }
    value.second = 0;
    state.rtc_setup.error = ESP_OK;
    state.current_page = PageId::SetDateTime;
    return ESP_OK;
}

esp_err_t save_rtc_setup(AppState &state)
{
    state.rtc_setup.value.second = 0;
    const esp_err_t result = sticky_rtc_set(state.rtc_setup.value);
    state.rtc_setup.error = result;
    if (result != ESP_OK) {
        return result;
    }
    open_name_input(state, state.rtc_setup.pending_type);
    return ESP_OK;
}

void append_create_entry_character(AppState &state, char character)
{
    CreateEntryState &entry = state.create_entry;
    if (entry.length + 1U >= sizeof(entry.name)) {
        entry.error = ESP_ERR_INVALID_SIZE;
        return;
    }
    if (entry.uppercase && character >= 'a' && character <= 'z') {
        character = static_cast<char>(character - 'a' + 'A');
    }
    entry.name[entry.length++] = character;
    entry.name[entry.length] = '\0';
    entry.error = ESP_OK;
}

void backspace_create_entry_name(AppState &state)
{
    CreateEntryState &entry = state.create_entry;
    if (entry.length > 0) {
        --entry.length;
        entry.name[entry.length] = '\0';
    }
    entry.error = ESP_OK;
}

void select_create_file_extension(AppState &state, size_t extension_index)
{
    if (state.create_entry.type != CreateEntryType::File ||
        extension_index >=
            sizeof(kFileExtensions) / sizeof(kFileExtensions[0])) {
        return;
    }
    state.create_entry.extension =
        static_cast<CreateFileExtension>(extension_index);
    state.create_entry.error = ESP_OK;
}

esp_err_t create_entry(AppState &state)
{
    CreateEntryState &entry = state.create_entry;
    if (entry.length == 0 || std::strcmp(entry.name, ".") == 0 ||
        std::strcmp(entry.name, "..") == 0 ||
        std::strchr(entry.name, '/') != nullptr ||
        std::strchr(entry.name, '\\') != nullptr) {
        entry.error = ESP_ERR_INVALID_ARG;
        return entry.error;
    }

    char final_name[kCreateEntryNameSize] = {};
    const char *name = entry.name;
    if (entry.type == CreateEntryType::File) {
        const size_t extension_index =
            static_cast<size_t>(entry.extension);
        if (extension_index >=
            sizeof(kFileExtensions) / sizeof(kFileExtensions[0])) {
            entry.error = ESP_ERR_INVALID_ARG;
            return entry.error;
        }
        const char *extension = kFileExtensions[extension_index];
        const size_t extension_length = std::strlen(extension);
        if (entry.length + 1U + extension_length >= sizeof(final_name)) {
            entry.error = ESP_ERR_INVALID_SIZE;
            return entry.error;
        }
        std::memcpy(final_name, entry.name, entry.length);
        final_name[entry.length] = '.';
        std::memcpy(final_name + entry.length + 1U,
                    extension,
                    extension_length + 1U);
        name = final_name;
    }

    const esp_err_t result = sticky_sdcard_create_entry(
        state.files.path,
        name,
        entry.type == CreateEntryType::Folder);
    entry.error = result;
    if (result != ESP_OK) {
        return result;
    }

    reload_file_browser(state);
    update_sd_card_info(state);
    state.current_page = PageId::Files;
    return ESP_OK;
}
