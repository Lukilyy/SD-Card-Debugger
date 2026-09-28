#pragma once

#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

enum class AppEventType {
    PreviousPage,
    NextPage,
    RefreshPage,
    ToggleCapacityUnits,
    OpenFiles,
    OpenHelp,
    SelectHelpAbout,
    SelectHelpGuide,
    OpenTools,
    OpenDiagnostics,
    OpenStorageTest,
    StartStorageTest,
    OpenClearCard,
    ProceedClearCard,
    ConfirmClearCard,
    CancelClearCard,
    OpenInitializeCard,
    ProceedInitializeCard,
    ConfirmInitializeCard,
    CancelInitializeCard,
    PreviousDiagnosticsPage,
    NextDiagnosticsPage,
    GoBack,
    PreviousFilePage,
    NextFilePage,
    OpenFileRow,
    OpenFolderDetails,
    RequestFileDelete,
    ConfirmFileDelete,
    CancelFileDelete,
    OpenNewEntryMenu,
    ChooseNewFolder,
    ChooseNewFile,
    AdjustDateTime,
    OpenDateTimeField,
    InputDateTimeDigit,
    DeleteDateTimeDigit,
    ConfirmDateTimeDigit,
    CancelDateTimeDigit,
    SaveDateTime,
    CancelDateTime,
    InputNameCharacter,
    BackspaceName,
    KeyboardInputRefresh,
    ToggleNameCase,
    SelectFileExtension,
    SubmitCreateEntry,
    CancelCreateEntry,
    SwipeUp,
    SdCardChanged,
    BatteryLogTick,
    ChargingStateChanged,
    EnterDeepSleep,
};

struct AppEvent {
    AppEventType type = AppEventType::RefreshPage;
    uint16_t x = 0;
    uint16_t y = 0;
};

// Serialized application event queue shared by physical buttons and touch.
esp_err_t app_event_init();
bool app_event_post(AppEventType type, uint16_t x = 0, uint16_t y = 0);
bool app_event_wait(AppEvent &event, TickType_t timeout);
