#include "touch_input.h"

#include <atomic>
#include <cstddef>

#include "app_event.h"
#include "app_state.h"
#include "esp_log.h"
#include "file_detail_page.h"
#include "file_page.h"
#include "help_page.h"
#include "home_page.h"
#include "new_entry_page.h"
#include "rtc_setup_page.h"
#include "tools_page.h"

namespace {

constexpr char kTag[] = "touch_input";

std::atomic<PageId> s_page{PageId::SdInfo};
std::atomic<size_t> s_file_entry_count{0};
std::atomic<size_t> s_file_page_index{0};
std::atomic<size_t> s_file_page_count{1};
std::atomic<bool> s_files_valid{false};
std::atomic<uint8_t> s_directory_mask{0};
std::atomic<bool> s_creating_file{false};
std::atomic<size_t> s_diagnostics_page{0};
std::atomic<size_t> s_diagnostics_page_count{1};

bool post_action(AppEventType type, uint16_t value = 0)
{
    if (app_event_post(type, value, 0)) {
        return true;
    }
    ESP_LOGW(kTag, "Action queue full; input was not accepted");
    return false;
}

}  // namespace

void touch_input_publish(const AppState &state)
{
    s_file_entry_count.store(state.files.entry_count,
                             std::memory_order_relaxed);
    s_file_page_index.store(state.files.page_index,
                            std::memory_order_relaxed);
    s_file_page_count.store(state.files.page_count,
                            std::memory_order_relaxed);
    s_files_valid.store(state.files.valid, std::memory_order_relaxed);
    uint8_t directory_mask = 0;
    for (size_t index = 0; index < state.files.entry_count && index < 8U;
         ++index) {
        if (state.files.entries[index].is_directory) {
            directory_mask |= static_cast<uint8_t>(1U << index);
        }
    }
    s_directory_mask.store(directory_mask, std::memory_order_relaxed);
    s_creating_file.store(
        state.create_entry.type == CreateEntryType::File,
        std::memory_order_relaxed);
    s_diagnostics_page.store(state.diagnostics_page,
                             std::memory_order_relaxed);
    s_diagnostics_page_count.store(diagnostics_page_count(state),
                                   std::memory_order_relaxed);
    s_page.store(state.current_page, std::memory_order_release);
}

bool touch_input_post_press(uint16_t x, uint16_t y)
{
    const PageId page = s_page.load(std::memory_order_acquire);
    if (page == PageId::SdInfo) {
        if (home_page_capacity_units_hit_test(x, y)) {
            return post_action(AppEventType::ToggleCapacityUnits);
        }
        if (home_page_help_hit_test(x, y)) {
            return post_action(AppEventType::OpenHelp);
        }
        if (home_page_files_hit_test(x, y)) {
            return post_action(AppEventType::OpenFiles);
        }
        if (home_page_tools_hit_test(x, y)) {
            return post_action(AppEventType::OpenTools);
        }
        return false;
    }

    if (page == PageId::Help) {
        if (help_page_back_hit_test(x, y)) {
            return post_action(AppEventType::GoBack);
        }
        if (help_page_about_hit_test(x, y)) {
            return post_action(AppEventType::SelectHelpAbout);
        }
        return help_page_guide_hit_test(x, y)
                   ? post_action(AppEventType::SelectHelpGuide) : false;
    }

    if (page == PageId::Tools) {
        if (tools_page_back_hit_test(x, y)) {
            return post_action(AppEventType::GoBack);
        }
        if (tools_page_diagnostics_hit_test(x, y)) {
            return post_action(AppEventType::OpenDiagnostics);
        }
        if (tools_page_storage_test_hit_test(x, y)) {
            return post_action(AppEventType::OpenStorageTest);
        }
        if (tools_page_clear_card_hit_test(x, y)) {
            return post_action(AppEventType::OpenClearCard);
        }
        return tools_page_initialize_card_hit_test(x, y)
                   ? post_action(AppEventType::OpenInitializeCard) : false;
    }

    if (page == PageId::Diagnostics) {
        if (diagnostics_page_back_hit_test(x, y)) {
            return post_action(AppEventType::GoBack);
        }
        const size_t page_index =
            s_diagnostics_page.load(std::memory_order_relaxed);
        const size_t page_count =
            s_diagnostics_page_count.load(std::memory_order_relaxed);
        if (diagnostics_page_prev_hit_test(x, y) && page_index > 0U) {
            return post_action(AppEventType::PreviousDiagnosticsPage);
        }
        return diagnostics_page_next_hit_test(x, y) &&
                   page_index + 1U < page_count
                   ? post_action(AppEventType::NextDiagnosticsPage)
                   : false;
    }

    if (page == PageId::StorageTest) {
        if (storage_test_page_back_hit_test(x, y)) {
            return post_action(AppEventType::GoBack);
        }
        return storage_test_page_start_hit_test(x, y)
                   ? post_action(AppEventType::StartStorageTest) : false;
    }

    if (page == PageId::ClearCard) {
        if (clear_card_page_back_hit_test(x, y)) {
            return post_action(AppEventType::GoBack);
        }
        return clear_card_page_action_hit_test(x, y)
                   ? post_action(AppEventType::ProceedClearCard) : false;
    }

    if (page == PageId::ClearCardConfirm) {
        if (clear_card_page_back_hit_test(x, y) ||
            clear_card_confirm_cancel_hit_test(x, y)) {
            return post_action(AppEventType::CancelClearCard);
        }
        return clear_card_confirm_clear_hit_test(x, y)
                   ? post_action(AppEventType::ConfirmClearCard) : false;
    }

    if (page == PageId::InitializeCard) {
        if (initialize_card_page_back_hit_test(x, y)) {
            return post_action(AppEventType::GoBack);
        }
        return initialize_card_page_action_hit_test(x, y)
                   ? post_action(AppEventType::ProceedInitializeCard) : false;
    }

    if (page == PageId::InitializeCardConfirm) {
        if (initialize_card_page_back_hit_test(x, y) ||
            initialize_card_confirm_cancel_hit_test(x, y)) {
            return post_action(AppEventType::CancelInitializeCard);
        }
        return initialize_card_confirm_start_hit_test(x, y)
                   ? post_action(AppEventType::ConfirmInitializeCard) : false;
    }

    if (page == PageId::FileDetails) {
        if (file_detail_back_hit_test(x, y)) {
            return post_action(AppEventType::GoBack);
        }
        if (file_detail_delete_hit_test(x, y)) {
            return post_action(AppEventType::RequestFileDelete);
        }
        return false;
    }

    if (page == PageId::DeleteConfirm) {
        if (file_detail_back_hit_test(x, y) ||
            delete_confirm_cancel_hit_test(x, y)) {
            return post_action(AppEventType::CancelFileDelete);
        }
        if (delete_confirm_delete_hit_test(x, y)) {
            return post_action(AppEventType::ConfirmFileDelete);
        }
        return false;
    }

    if (page == PageId::NewEntryMenu) {
        if (new_entry_back_hit_test(x, y)) {
            return post_action(AppEventType::GoBack);
        }
        if (new_folder_hit_test(x, y)) {
            return post_action(AppEventType::ChooseNewFolder);
        }
        if (new_file_hit_test(x, y)) {
            return post_action(AppEventType::ChooseNewFile);
        }
        return false;
    }

    if (page == PageId::NameInput) {
        if (new_entry_back_hit_test(x, y) ||
            name_input_cancel_hit_test(x, y)) {
            return post_action(AppEventType::CancelCreateEntry);
        }
        if (name_input_case_hit_test(x, y)) {
            return post_action(AppEventType::ToggleNameCase);
        }
        if (name_input_backspace_hit_test(x, y)) {
            return post_action(AppEventType::BackspaceName);
        }
        if (name_input_create_hit_test(x, y)) {
            return post_action(AppEventType::SubmitCreateEntry);
        }
        if (s_creating_file.load(std::memory_order_relaxed)) {
            const int extension = name_input_extension_at(x, y);
            if (extension >= 0) {
                return post_action(AppEventType::SelectFileExtension,
                                   static_cast<uint16_t>(extension));
            }
        }
        const char character = name_input_character_at(x, y);
        return character != '\0'
                   ? post_action(AppEventType::InputNameCharacter,
                                 static_cast<uint16_t>(character))
                   : false;
    }

    if (page == PageId::SetDateTime) {
        if (rtc_setup_cancel_hit_test(x, y)) {
            return post_action(AppEventType::CancelDateTime);
        }
        if (rtc_setup_save_hit_test(x, y)) {
            return post_action(AppEventType::SaveDateTime);
        }
        DateTimeField value_field = DateTimeField::Year;
        if (rtc_setup_value_field_at(x, y, value_field)) {
            return post_action(
                AppEventType::OpenDateTimeField,
                static_cast<uint16_t>(value_field));
        }
        DateTimeField field = DateTimeField::Year;
        int direction = 0;
        if (rtc_setup_adjustment_at(x, y, field, direction)) {
            return app_event_post(
                AppEventType::AdjustDateTime,
                static_cast<uint16_t>(field),
                direction < 0 ? 0U : 1U);
        }
        return false;
    }

    if (page == PageId::SetDateTimeNumber) {
        if (rtc_number_input_cancel_hit_test(x, y)) {
            return post_action(AppEventType::CancelDateTimeDigit);
        }
        if (rtc_number_input_delete_hit_test(x, y)) {
            return post_action(AppEventType::DeleteDateTimeDigit);
        }
        if (rtc_number_input_ok_hit_test(x, y)) {
            return post_action(AppEventType::ConfirmDateTimeDigit);
        }
        const char digit = rtc_number_input_digit_at(x, y);
        return digit != '\0'
                   ? post_action(AppEventType::InputDateTimeDigit,
                                 static_cast<uint16_t>(digit))
                   : false;
    }

    if (file_page_back_hit_test(x, y)) {
        return post_action(AppEventType::GoBack);
    }
    if (!s_files_valid.load(std::memory_order_relaxed)) {
        return false;
    }
    if (file_page_new_hit_test(x, y)) {
        return post_action(AppEventType::OpenNewEntryMenu);
    }
    const size_t page_index =
        s_file_page_index.load(std::memory_order_relaxed);
    const size_t page_count =
        s_file_page_count.load(std::memory_order_relaxed);
    if (file_page_prev_hit_test(x, y) && page_index > 0U) {
        return post_action(AppEventType::PreviousFilePage);
    }
    if (file_page_next_hit_test(x, y) && page_index + 1U < page_count) {
        return post_action(AppEventType::NextFilePage);
    }
    return false;
}

bool touch_input_post_tap(uint16_t x, uint16_t y)
{
    if (s_page.load(std::memory_order_acquire) != PageId::Files ||
        !s_files_valid.load(std::memory_order_relaxed)) {
        return false;
    }

    const int row = file_page_row_at(x, y);
    const size_t entry_count =
        s_file_entry_count.load(std::memory_order_relaxed);
    if (row < 0 || static_cast<size_t>(row) >= entry_count) {
        return false;
    }
    const uint8_t directory_mask =
        s_directory_mask.load(std::memory_order_relaxed);
    if (file_page_info_hit_test(x, y)) {
        return (directory_mask & static_cast<uint8_t>(1U << row)) != 0U
                   ? post_action(AppEventType::OpenFolderDetails,
                                 static_cast<uint16_t>(row))
                   : post_action(AppEventType::OpenFileRow,
                                 static_cast<uint16_t>(row));
    }
    return post_action(AppEventType::OpenFileRow,
                       static_cast<uint16_t>(row));
}
