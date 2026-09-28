#include "app.h"

#include <atomic>
#include <cstring>

#include "app_battery.h"
#include "app_data.h"
#include "app_file_data.h"
#include "canvas.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "file_detail_page.h"
#include "file_page.h"
#include "help_page.h"
#include "home_page.h"
#include "new_entry_page.h"
#include "rtc_setup_page.h"
#include "sleep_page.h"
#include "tools_page.h"
#include "sticky_buzzer.h"
#include "sticky_display.h"
#include "sticky_power.h"
#include "sticky_touch.h"
#include "touch_input.h"

namespace {

constexpr char kTag[] = "app";
constexpr uint32_t kPartialRefreshesBeforeCleanup = 15;
constexpr int64_t kKeyboardRefreshDelayUs = 300000;
uint32_t s_partial_refresh_count = 0;
esp_timer_handle_t s_keyboard_refresh_timer = nullptr;
std::atomic<uint32_t> s_keyboard_refresh_generation{0};
std::atomic<uint32_t> s_keyboard_refresh_deadline_ms{0};
std::atomic<uint16_t> s_keyboard_refresh_page{
    static_cast<uint16_t>(PageId::SdInfo)};

void keyboard_refresh_timer_callback(void *)
{
    const uint16_t generation = static_cast<uint16_t>(
        s_keyboard_refresh_generation.load(std::memory_order_acquire));
    const uint16_t page =
        s_keyboard_refresh_page.load(std::memory_order_relaxed);
    if (!app_event_post(AppEventType::KeyboardInputRefresh,
                        generation, page)) {
        ESP_LOGW(kTag, "Keyboard refresh event queue is full");
    }
}

esp_err_t schedule_keyboard_refresh(PageId page)
{
    if (s_keyboard_refresh_timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = keyboard_refresh_timer_callback;
        args.name = "keyboard_refresh";
        args.skip_unhandled_events = true;
        const esp_err_t create_result =
            esp_timer_create(&args, &s_keyboard_refresh_timer);
        if (create_result != ESP_OK) {
            return create_result;
        }
    }

    s_keyboard_refresh_page.store(
        static_cast<uint16_t>(page), std::memory_order_relaxed);
    const uint32_t now_ms =
        static_cast<uint32_t>(esp_timer_get_time() / 1000);
    s_keyboard_refresh_deadline_ms.store(
        now_ms + static_cast<uint32_t>(kKeyboardRefreshDelayUs / 1000),
        std::memory_order_relaxed);
    s_keyboard_refresh_generation.fetch_add(1U, std::memory_order_release);

    const esp_err_t stop_result = esp_timer_stop(s_keyboard_refresh_timer);
    if (stop_result != ESP_OK && stop_result != ESP_ERR_INVALID_STATE) {
        s_keyboard_refresh_generation.fetch_add(1U,
                                                std::memory_order_acq_rel);
        s_keyboard_refresh_deadline_ms.store(0, std::memory_order_release);
        return stop_result;
    }
    const esp_err_t start_result = esp_timer_start_once(
        s_keyboard_refresh_timer, kKeyboardRefreshDelayUs);
    if (start_result != ESP_OK) {
        s_keyboard_refresh_generation.fetch_add(1U,
                                                std::memory_order_acq_rel);
        s_keyboard_refresh_deadline_ms.store(0, std::memory_order_release);
    }
    return start_result;
}

void cancel_keyboard_refresh()
{
    s_keyboard_refresh_generation.fetch_add(1U, std::memory_order_acq_rel);
    s_keyboard_refresh_deadline_ms.store(0, std::memory_order_release);
    if (s_keyboard_refresh_timer != nullptr) {
        const esp_err_t result = esp_timer_stop(s_keyboard_refresh_timer);
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(kTag, "Stop keyboard refresh timer failed: %s",
                     esp_err_to_name(result));
        }
    }
}

bool consume_keyboard_refresh(const AppState &state, const AppEvent &event)
{
    const uint16_t generation = static_cast<uint16_t>(
        s_keyboard_refresh_generation.load(std::memory_order_acquire));
    const uint32_t deadline_ms =
        s_keyboard_refresh_deadline_ms.load(std::memory_order_acquire);
    const uint32_t now_ms =
        static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (event.x != generation ||
        event.y != static_cast<uint16_t>(state.current_page) ||
        deadline_ms == 0 ||
        static_cast<int32_t>(now_ms - deadline_ms) < 0) {
        return false;
    }
    s_keyboard_refresh_generation.fetch_add(1U, std::memory_order_acq_rel);
    s_keyboard_refresh_deadline_ms.store(0, std::memory_order_release);
    return true;
}

esp_err_t refresh_with_cleanup_policy()
{
    if (s_partial_refresh_count >= kPartialRefreshesBeforeCleanup) {
        s_partial_refresh_count = 0;
        ESP_LOGI(kTag, "Running full refresh to clear e-paper ghosting");
        return sticky_display_refresh_monochrome();
    }
    ++s_partial_refresh_count;
    return sticky_display_refresh_partial();
}

esp_err_t render_and_refresh(Canvas &canvas, const AppState &state)
{
    render_current_page(canvas, state);
    return refresh_with_cleanup_policy();
}

esp_err_t render_refresh_and_beep(Canvas &canvas, const AppState &state)
{
    const esp_err_t beep_result = sticky_buzzer_beep();
    if (beep_result != ESP_OK) {
        ESP_LOGW(kTag, "Button feedback beep failed: %s",
                 esp_err_to_name(beep_result));
    }
    return render_and_refresh(canvas, state);
}

esp_err_t refresh_file_selection_and_beep(Canvas &canvas,
                                          const AppState &state,
                                          size_t previous_selection)
{
    file_page_render_selection(canvas, state, previous_selection);
    const esp_err_t beep_result = sticky_buzzer_beep();
    if (beep_result != ESP_OK) {
        ESP_LOGW(kTag, "Button feedback beep failed: %s",
                 esp_err_to_name(beep_result));
    }
    ESP_RETURN_ON_ERROR(
        refresh_with_cleanup_policy(), kTag, "refresh file selection");
    return ESP_OK;
}

esp_err_t refresh_name_input_value(Canvas &canvas, const AppState &state)
{
    name_input_page_render_value(canvas, state);
    return refresh_with_cleanup_policy();
}

esp_err_t refresh_rtc_number_input_value(Canvas &canvas,
                                         const AppState &state)
{
    rtc_number_input_page_render_value(canvas, state);
    return refresh_with_cleanup_policy();
}

esp_err_t refresh_help_battery_status(Canvas &canvas, const AppState &state)
{
    help_page_render_battery_status(canvas, state);
    return refresh_with_cleanup_policy();
}

}  // namespace

esp_err_t refresh_current_page(const AppState &)
{
    s_partial_refresh_count = 0;
    return sticky_display_refresh_monochrome();
}

void render_current_page(Canvas &canvas, const AppState &state)
{
    touch_input_publish(state);
    switch (state.current_page) {
    case PageId::SdInfo:
        home_page_render(canvas, state);
        break;
    case PageId::Files:
        file_page_render(canvas, state);
        break;
    case PageId::FileDetails:
        file_detail_page_render(canvas, state);
        break;
    case PageId::DeleteConfirm:
        delete_confirm_page_render(canvas, state);
        break;
    case PageId::NewEntryMenu:
        new_entry_menu_render(canvas);
        break;
    case PageId::SetDateTime:
        rtc_setup_page_render(canvas, state);
        break;
    case PageId::SetDateTimeNumber:
        rtc_number_input_page_render(canvas, state);
        break;
    case PageId::NameInput:
        name_input_page_render(canvas, state);
        break;
    case PageId::Help:
        help_page_render(canvas, state);
        break;
    case PageId::Tools:
        tools_page_render(canvas);
        break;
    case PageId::Diagnostics:
        diagnostics_page_render(canvas, state);
        break;
    case PageId::StorageTest:
        storage_test_page_render(canvas, state);
        break;
    case PageId::ClearCard:
        clear_card_page_render(canvas, state);
        break;
    case PageId::ClearCardConfirm:
        clear_card_confirm_page_render(canvas);
        break;
    case PageId::InitializeCard:
        initialize_card_page_render(canvas, state);
        break;
    case PageId::InitializeCardConfirm:
        initialize_card_confirm_page_render(canvas, state);
        break;
    }
}

esp_err_t handle_app_event(Canvas &canvas,
                           AppState &state,
                           const AppEvent &event)
{
    const bool keyboard_page =
        state.current_page == PageId::NameInput ||
        state.current_page == PageId::SetDateTimeNumber;
    const bool keyboard_edit_event =
        event.type == AppEventType::InputNameCharacter ||
        event.type == AppEventType::BackspaceName ||
        event.type == AppEventType::InputDateTimeDigit ||
        event.type == AppEventType::DeleteDateTimeDigit;
    const bool passive_event =
        event.type == AppEventType::BatteryLogTick ||
        event.type == AppEventType::ChargingStateChanged;
    if (keyboard_page && !keyboard_edit_event &&
        event.type != AppEventType::KeyboardInputRefresh && !passive_event) {
        cancel_keyboard_refresh();
    }

    if (event.type == AppEventType::KeyboardInputRefresh) {
        if (!keyboard_page || !consume_keyboard_refresh(state, event)) {
            return ESP_OK;
        }
        return state.current_page == PageId::NameInput
                   ? refresh_name_input_value(canvas, state)
                   : refresh_rtc_number_input_value(canvas, state);
    }

    if (event.type == AppEventType::BatteryLogTick) {
        const bool previous_valid = state.battery.valid;
        const int previous_percent = state.battery.percent;
        const bool previous_charging_valid = state.battery.charging_valid;
        const bool previous_charging = state.battery.charging;
        app_battery_read(state.battery);
        app_battery_log(state.battery);
        const bool percentage_changed =
            previous_valid != state.battery.valid ||
            (state.battery.valid && previous_percent != state.battery.percent);
        const bool charging_changed =
            previous_charging_valid != state.battery.charging_valid ||
            (state.battery.charging_valid &&
             previous_charging != state.battery.charging);
        if (state.current_page == PageId::Help &&
            (percentage_changed || charging_changed)) {
            return refresh_help_battery_status(canvas, state);
        }
        return ESP_OK;
    }


    if (event.type == AppEventType::ChargingStateChanged) {
        bool charging = false;
        if (!app_battery_read_charging(charging)) {
            return ESP_OK;
        }
        const bool changed = !state.battery.charging_valid ||
                             state.battery.charging != charging;
        state.battery.charging = charging;
        state.battery.charging_valid = true;
        return changed && state.current_page == PageId::Help
                   ? refresh_help_battery_status(canvas, state)
                   : ESP_OK;
    }

    if (event.type == AppEventType::EnterDeepSleep) {
        ESP_LOGI(kTag, "Preparing for deep sleep");
        const esp_err_t beep_result = sticky_buzzer_beep();
        if (beep_result != ESP_OK) {
            ESP_LOGW(kTag, "Deep-sleep feedback beep failed: %s",
                     esp_err_to_name(beep_result));
        }
        sleep_page_render(canvas);
        ESP_RETURN_ON_ERROR(
            sticky_display_refresh_monochrome(), kTag, "refresh sleep page");
        ESP_RETURN_ON_ERROR(sticky_touch_stop(), kTag, "stop touch polling");
        ESP_RETURN_ON_ERROR(sticky_display_sleep(), kTag, "sleep display");
        sticky_power_enter_deep_sleep();
    }

    if (event.type == AppEventType::SdCardChanged) {
        if (state.current_page == PageId::Diagnostics) {
            run_raw_diagnostics(state);
            return render_and_refresh(canvas, state);
        }
        if (state.current_page == PageId::InitializeCard ||
            state.current_page == PageId::InitializeCardConfirm) {
            handle_sd_card_change(state);
            state.initialize_card = {};
            state.initialize_running = false;
            if (state.sd_card.inserted) {
                prepare_initialize_card(state);
            }
            state.current_page = PageId::InitializeCard;
            return render_and_refresh(canvas, state);
        }
        handle_sd_card_change(state);
        if (state.current_page == PageId::Help) {
            return ESP_OK;
        }
        return render_and_refresh(canvas, state);
    }

    if (state.current_page == PageId::SdInfo) {
        if (event.type == AppEventType::ToggleCapacityUnits) {
            state.capacity_unit_mode =
                state.capacity_unit_mode == CapacityUnitMode::Decimal
                    ? CapacityUnitMode::Binary
                    : CapacityUnitMode::Decimal;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::RefreshPage) {
            update_sd_card_info(state);
            return render_refresh_and_beep(canvas, state);
        }
        if (event.type == AppEventType::OpenFiles) {
            open_file_browser(state);
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::OpenHelp) {
            app_battery_read(state.battery);
            state.help_section = HelpSection::About;
            state.current_page = PageId::Help;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::OpenTools) {
            state.current_page = PageId::Tools;
            return render_and_refresh(canvas, state);
        }
        return ESP_OK;
    }

    if (state.current_page == PageId::Help) {
        if (event.type == AppEventType::GoBack ||
            event.type == AppEventType::SwipeUp) {
            state.current_page = PageId::SdInfo;
            return event.type == AppEventType::GoBack
                       ? render_and_refresh(canvas, state)
                       : render_refresh_and_beep(canvas, state);
        }
        if (event.type == AppEventType::SelectHelpAbout ||
            event.type == AppEventType::PreviousPage) {
            if (state.help_section == HelpSection::About) {
                return ESP_OK;
            }
            state.help_section = HelpSection::About;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::SelectHelpGuide ||
            event.type == AppEventType::NextPage) {
            if (state.help_section == HelpSection::Guide) {
                return ESP_OK;
            }
            state.help_section = HelpSection::Guide;
            return render_and_refresh(canvas, state);
        }
        return ESP_OK;
    }

    if (state.current_page == PageId::Tools) {
        if (event.type == AppEventType::GoBack ||
            event.type == AppEventType::SwipeUp) {
            state.current_page = PageId::SdInfo;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::OpenDiagnostics ||
            event.type == AppEventType::RefreshPage) {
            run_raw_diagnostics(state);
            state.current_page = PageId::Diagnostics;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::OpenStorageTest) {
            state.storage_test = {};
            state.current_page = PageId::StorageTest;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::OpenClearCard) {
            state.clear_card = {};
            state.current_page = PageId::ClearCard;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::OpenInitializeCard) {
            state.initialize_card = {};
            state.initialize_running = false;
            prepare_initialize_card(state);
            state.current_page = PageId::InitializeCard;
            return render_and_refresh(canvas, state);
        }
        return ESP_OK;
    }

    if (state.current_page == PageId::Diagnostics) {
        if (event.type == AppEventType::GoBack) {
            state.current_page = PageId::Tools;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::SwipeUp) {
            state.current_page = PageId::SdInfo;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::RefreshPage) {
            run_raw_diagnostics(state);
            return render_and_refresh(canvas, state);
        }
        if ((event.type == AppEventType::PreviousDiagnosticsPage ||
             event.type == AppEventType::PreviousPage) &&
            state.diagnostics_page > 0U) {
            --state.diagnostics_page;
            return render_and_refresh(canvas, state);
        }
        if ((event.type == AppEventType::NextDiagnosticsPage ||
             event.type == AppEventType::NextPage) &&
            state.diagnostics_page + 1U < diagnostics_page_count(state)) {
            ++state.diagnostics_page;
            return render_and_refresh(canvas, state);
        }
        return ESP_OK;
    }

    if (state.current_page == PageId::StorageTest) {
        if (event.type == AppEventType::GoBack) {
            state.current_page = PageId::Tools;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::SwipeUp) {
            state.current_page = PageId::SdInfo;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::StartStorageTest ||
            event.type == AppEventType::RefreshPage) {
            run_storage_test(state);
            return render_and_refresh(canvas, state);
        }
        return ESP_OK;
    }

    if (state.current_page == PageId::ClearCard) {
        if (event.type == AppEventType::GoBack) {
            state.current_page = PageId::Tools;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::SwipeUp) {
            state.current_page = PageId::SdInfo;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::ProceedClearCard ||
            event.type == AppEventType::RefreshPage) {
            state.current_page = state.clear_card.attempted
                                     ? PageId::Tools
                                     : PageId::ClearCardConfirm;
            return render_and_refresh(canvas, state);
        }
        return ESP_OK;
    }

    if (state.current_page == PageId::ClearCardConfirm) {
        if (event.type == AppEventType::CancelClearCard ||
            event.type == AppEventType::GoBack) {
            state.current_page = PageId::ClearCard;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::SwipeUp) {
            state.current_page = PageId::SdInfo;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::ConfirmClearCard) {
            run_clear_card(state);
            state.current_page = PageId::ClearCard;
            return render_and_refresh(canvas, state);
        }
        return ESP_OK;
    }

    if (state.current_page == PageId::InitializeCard) {
        if (event.type == AppEventType::GoBack) {
            state.current_page = PageId::Tools;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::SwipeUp) {
            state.current_page = PageId::SdInfo;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::ProceedInitializeCard ||
            event.type == AppEventType::RefreshPage) {
            state.current_page = state.initialize_card.attempted
                                     ? PageId::Tools
                                     : PageId::InitializeCardConfirm;
            return render_and_refresh(canvas, state);
        }
        return ESP_OK;
    }

    if (state.current_page == PageId::InitializeCardConfirm) {
        if (event.type == AppEventType::CancelInitializeCard ||
            event.type == AppEventType::GoBack) {
            state.current_page = PageId::InitializeCard;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::SwipeUp) {
            state.current_page = PageId::SdInfo;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::ConfirmInitializeCard) {
            state.initialize_running = true;
            state.current_page = PageId::InitializeCard;
            const esp_err_t refresh_result =
                render_and_refresh(canvas, state);
            if (refresh_result != ESP_OK) {
                state.initialize_running = false;
                return refresh_result;
            }
            run_initialize_card(state);
            state.initialize_running = false;
            return render_and_refresh(canvas, state);
        }
        return ESP_OK;
    }

    if (state.current_page == PageId::FileDetails) {
        if (event.type == AppEventType::GoBack) {
            state.current_page = PageId::Files;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::RequestFileDelete) {
            if (state.file_action.is_directory &&
                state.file_action.error != ESP_OK) {
                return render_and_refresh(canvas, state);
            }
            state.file_action.error = ESP_OK;
            state.current_page = PageId::DeleteConfirm;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::RefreshPage) {
            if (state.file_action.is_directory &&
                state.file_action.error != ESP_OK) {
                return render_refresh_and_beep(canvas, state);
            }
            state.file_action.error = ESP_OK;
            state.current_page = PageId::DeleteConfirm;
            return render_refresh_and_beep(canvas, state);
        }
        if (event.type == AppEventType::SwipeUp) {
            state.current_page = PageId::SdInfo;
            return render_refresh_and_beep(canvas, state);
        }
        return ESP_OK;
    }

    if (state.current_page == PageId::DeleteConfirm) {
        if (event.type == AppEventType::CancelFileDelete ||
            event.type == AppEventType::GoBack) {
            state.current_page = PageId::FileDetails;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::SwipeUp) {
            state.current_page = PageId::SdInfo;
            return render_refresh_and_beep(canvas, state);
        }
        if (event.type == AppEventType::ConfirmFileDelete ||
            event.type == AppEventType::RefreshPage) {
            const esp_err_t delete_result = state.file_action.is_directory
                                                ? delete_selected_folder(state)
                                                : delete_selected_file(state);
            if (delete_result != ESP_OK) {
                ESP_LOGW(kTag, "File delete failed: %s",
                         esp_err_to_name(delete_result));
            }
            return event.type == AppEventType::ConfirmFileDelete
                       ? render_and_refresh(canvas, state)
                       : render_refresh_and_beep(canvas, state);
        }
        return ESP_OK;
    }

    if (state.current_page == PageId::NewEntryMenu) {
        if (event.type == AppEventType::GoBack) {
            state.current_page = PageId::Files;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::ChooseNewFolder) {
            begin_create_entry(state, CreateEntryType::Folder);
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::ChooseNewFile) {
            begin_create_entry(state, CreateEntryType::File);
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::SwipeUp) {
            state.current_page = PageId::SdInfo;
            return render_refresh_and_beep(canvas, state);
        }
        return ESP_OK;
    }

    if (state.current_page == PageId::SetDateTime) {
        if (event.type == AppEventType::CancelDateTime ||
            event.type == AppEventType::GoBack) {
            state.current_page = PageId::NewEntryMenu;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::AdjustDateTime) {
            adjust_rtc_setup_value(
                state,
                static_cast<DateTimeField>(event.x),
                event.y == 0U ? -1 : 1);
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::OpenDateTimeField &&
            event.x <= static_cast<uint16_t>(DateTimeField::Minute)) {
            begin_date_time_number_input(
                state, static_cast<DateTimeField>(event.x));
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::SaveDateTime) {
            const esp_err_t result = save_rtc_setup(state);
            if (result != ESP_OK) {
                ESP_LOGW(kTag, "RTC setup failed: %s",
                         esp_err_to_name(result));
            }
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::SwipeUp) {
            state.current_page = PageId::SdInfo;
            return render_refresh_and_beep(canvas, state);
        }
        return ESP_OK;
    }

    if (state.current_page == PageId::SetDateTimeNumber) {
        if (event.type == AppEventType::CancelDateTimeDigit ||
            event.type == AppEventType::GoBack) {
            state.current_page = PageId::SetDateTime;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::InputDateTimeDigit) {
            append_date_time_digit(state, static_cast<char>(event.x));
            const esp_err_t result =
                schedule_keyboard_refresh(state.current_page);
            return result == ESP_OK
                       ? ESP_OK
                       : refresh_rtc_number_input_value(canvas, state);
        }
        if (event.type == AppEventType::DeleteDateTimeDigit) {
            delete_date_time_digit(state);
            const esp_err_t result =
                schedule_keyboard_refresh(state.current_page);
            return result == ESP_OK
                       ? ESP_OK
                       : refresh_rtc_number_input_value(canvas, state);
        }
        if (event.type == AppEventType::ConfirmDateTimeDigit) {
            const esp_err_t result = confirm_date_time_number_input(state);
            if (result != ESP_OK) {
                ESP_LOGW(kTag, "Invalid date/time field value");
            }
            return render_and_refresh(canvas, state);
        }
        return ESP_OK;
    }

    if (state.current_page == PageId::NameInput) {
        if (event.type == AppEventType::CancelCreateEntry ||
            event.type == AppEventType::GoBack) {
            state.current_page = PageId::NewEntryMenu;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::InputNameCharacter) {
            append_create_entry_character(
                state, static_cast<char>(event.x));
            const esp_err_t result =
                schedule_keyboard_refresh(state.current_page);
            return result == ESP_OK
                       ? ESP_OK
                       : refresh_name_input_value(canvas, state);
        }
        if (event.type == AppEventType::BackspaceName) {
            backspace_create_entry_name(state);
            const esp_err_t result =
                schedule_keyboard_refresh(state.current_page);
            return result == ESP_OK
                       ? ESP_OK
                       : refresh_name_input_value(canvas, state);
        }
        if (event.type == AppEventType::ToggleNameCase) {
            state.create_entry.uppercase = !state.create_entry.uppercase;
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::SelectFileExtension) {
            select_create_file_extension(state, event.x);
            return render_and_refresh(canvas, state);
        }
        if (event.type == AppEventType::SubmitCreateEntry ||
            event.type == AppEventType::RefreshPage) {
            const esp_err_t create_result = create_entry(state);
            if (create_result != ESP_OK) {
                ESP_LOGW(kTag, "Create entry failed: %s",
                         esp_err_to_name(create_result));
            }
            return event.type == AppEventType::SubmitCreateEntry
                       ? render_and_refresh(canvas, state)
                       : render_refresh_and_beep(canvas, state);
        }
        if (event.type == AppEventType::SwipeUp) {
            state.current_page = PageId::SdInfo;
            return render_refresh_and_beep(canvas, state);
        }
        return ESP_OK;
    }

    if (event.type == AppEventType::SwipeUp) {
        state.current_page = PageId::SdInfo;
        return render_refresh_and_beep(canvas, state);
    }

    if (event.type == AppEventType::PreviousPage) {
        const size_t previous_selection = state.files.selected;
        select_previous_file_entry(state);
        if (state.files.selected == previous_selection) {
            return ESP_OK;
        }
        return refresh_file_selection_and_beep(
            canvas, state, previous_selection);
    }
    if (event.type == AppEventType::NextPage) {
        const size_t previous_selection = state.files.selected;
        select_next_file_entry(state);
        if (state.files.selected == previous_selection) {
            return ESP_OK;
        }
        return refresh_file_selection_and_beep(
            canvas, state, previous_selection);
    }
    if (event.type == AppEventType::RefreshPage) {
        activate_selected_file_entry(state);
        return render_refresh_and_beep(canvas, state);
    }
    if (event.type == AppEventType::GoBack) {
        file_browser_go_parent(state);
        return render_and_refresh(canvas, state);
    }
    if (event.type == AppEventType::OpenNewEntryMenu) {
        open_new_entry_menu(state);
        return render_and_refresh(canvas, state);
    }
    if (event.type == AppEventType::PreviousFilePage) {
        const size_t previous_page = state.files.page_index;
        file_browser_previous_page(state);
        if (state.files.page_index == previous_page) {
            return ESP_OK;
        }
        return render_and_refresh(canvas, state);
    }
    if (event.type == AppEventType::NextFilePage) {
        const size_t previous_page = state.files.page_index;
        file_browser_next_page(state);
        if (state.files.page_index == previous_page) {
            return ESP_OK;
        }
        return render_and_refresh(canvas, state);
    }

    if ((event.type != AppEventType::OpenFileRow &&
         event.type != AppEventType::OpenFolderDetails) ||
        static_cast<size_t>(event.x) >= state.files.entry_count) {
        return ESP_OK;
    }
    state.files.selected = static_cast<size_t>(event.x);
    if (event.type == AppEventType::OpenFolderDetails) {
        open_selected_folder_details(state);
        return render_and_refresh(canvas, state);
    }
    activate_selected_file_entry(state);
    return render_and_refresh(canvas, state);
}
