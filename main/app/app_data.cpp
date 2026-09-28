#include "app_data.h"

#include <cstdio>
#include <cstring>

#include "app_state.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sticky_sdcard.h"

namespace {

constexpr char kTag[] = "app_data";

void merge_raw_home_info(StickySdCardInfo &info,
                         const StickySdRawDiagnostics &raw)
{
    if (!raw.inserted || !raw.card_initialized) {
        return;
    }

    info.inserted = true;
    info.raw_readable = true;
    info.total_bytes = raw.capacity_bytes;
    info.sector_count = raw.sector_count;
    info.sector_size = raw.sector_size;
    std::snprintf(info.card_type, sizeof(info.card_type), "%s",
                  raw.card_type);

    if (std::strcmp(raw.detected_file_system, "UNKNOWN") != 0) {
        std::snprintf(info.file_system, sizeof(info.file_system), "%s",
                      raw.detected_file_system);
    }

    switch (raw.scheme) {
    case StickySdPartitionScheme::Mbr:
        std::snprintf(info.partition, sizeof(info.partition), "MBR");
        break;
    case StickySdPartitionScheme::Gpt:
        std::snprintf(info.partition, sizeof(info.partition), "%s",
                      raw.gpt_valid ? "GPT" : "GPT INVALID");
        break;
    case StickySdPartitionScheme::Superfloppy:
        std::snprintf(info.partition, sizeof(info.partition), "SUPERFLOPPY");
        break;
    case StickySdPartitionScheme::Unknown:
        break;
    }

    for (size_t index = 0; index < raw.detected_partition_count; ++index) {
        const StickySdDetectedPartition &partition =
            raw.detected_partitions[index];
        if (partition.cluster_size != 0U &&
            (std::strcmp(partition.file_system, info.file_system) == 0 ||
             info.cluster_size == 0U)) {
            info.cluster_size = partition.cluster_size;
            if (std::strcmp(partition.file_system, info.file_system) == 0) {
                break;
            }
        }
    }
}

}  // namespace

void update_sd_card_info(AppState &state)
{
    ESP_LOGI(kTag, "Reading SD card information");
    const esp_err_t result = sticky_sdcard_get_info(state.sd_card);
    if (result == ESP_OK || !state.sd_card.inserted) {
        return;
    }

    ESP_LOGW(kTag, "SD card mount query failed: %s; trying raw fallback",
             esp_err_to_name(result));
    StickySdRawDiagnostics *raw =
        static_cast<StickySdRawDiagnostics *>(heap_caps_calloc(
            1U, sizeof(StickySdRawDiagnostics), MALLOC_CAP_8BIT));
    if (raw == nullptr) {
        ESP_LOGE(kTag, "Home raw fallback allocation failed");
        return;
    }

    const esp_err_t raw_result = sticky_sdcard_get_raw_diagnostics(*raw);
    merge_raw_home_info(state.sd_card, *raw);
    if (raw_result != ESP_OK) {
        ESP_LOGW(kTag, "Home raw fallback incomplete: %s",
                 esp_err_to_name(raw_result));
    } else {
        ESP_LOGI(kTag,
                 "Home raw fallback: card readable, filesystem=%s partition=%s",
                 state.sd_card.file_system, state.sd_card.partition);
    }
    heap_caps_free(raw);
}

void collect_startup_sd_report(AppState &state)
{
    StickySdRawDiagnostics *raw =
        static_cast<StickySdRawDiagnostics *>(heap_caps_calloc(
            1U, sizeof(StickySdRawDiagnostics), MALLOC_CAP_8BIT));
    if (raw == nullptr) {
        ESP_LOGE(kTag, "Startup SD report allocation failed");
        update_sd_card_info(state);
        return;
    }

    if (!sticky_sdcard_is_inserted()) {
        raw->inserted = false;
        raw->error = ESP_ERR_NOT_FOUND;
        state.sd_card = {};
        state.sd_card.error = ESP_ERR_NOT_FOUND;
        state.diagnostics = *raw;
        sticky_sdcard_log_report(*raw, state.sd_card);
        heap_caps_free(raw);
        return;
    }

    const esp_err_t raw_result = sticky_sdcard_get_raw_diagnostics(*raw);
    StickySdCardInfo mount_info = {};
    if (raw->inserted) {
        raw->mount_checked = true;
        raw->mount_error = sticky_sdcard_get_info(mount_info);
        raw->mount_ok = raw->mount_error == ESP_OK && mount_info.mounted;
        if (raw->mount_ok &&
            std::strcmp(raw->detected_file_system, "UNKNOWN") == 0) {
            std::snprintf(raw->detected_file_system,
                          sizeof(raw->detected_file_system), "%s",
                          mount_info.file_system);
        }
    } else {
        mount_info.inserted = raw->inserted;
        mount_info.error = raw_result;
    }

    state.sd_card = mount_info;
    if (!raw->mount_ok) {
        merge_raw_home_info(state.sd_card, *raw);
        state.sd_card.error = raw->mount_checked
                                  ? raw->mount_error
                                  : raw_result;
    }
    state.diagnostics = *raw;
    sticky_sdcard_log_report(*raw, mount_info);
    heap_caps_free(raw);
}

void run_raw_diagnostics(AppState &state)
{
    state.diagnostics_page = 0;
    const esp_err_t result =
        sticky_sdcard_get_raw_diagnostics(state.diagnostics);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Raw diagnostics failed: %s",
                 esp_err_to_name(result));
    }
    if (state.diagnostics.card_initialized && state.diagnostics.inserted) {
        StickySdCardInfo mount_info = {};
        state.diagnostics.mount_checked = true;
        state.diagnostics.mount_error = sticky_sdcard_get_info(mount_info);
        state.diagnostics.mount_ok =
            state.diagnostics.mount_error == ESP_OK && mount_info.mounted;
        if (state.diagnostics.mount_ok) {
            state.sd_card = mount_info;
            if (std::strcmp(state.diagnostics.detected_file_system,
                            "UNKNOWN") == 0) {
                std::snprintf(state.diagnostics.detected_file_system,
                              sizeof(state.diagnostics.detected_file_system),
                              "%s", mount_info.file_system);
            }
        }
        ESP_LOGI(kTag, "Diagnostics mount: %s (%s)",
                 state.diagnostics.mount_ok ? "OK" : "FAILED",
                 esp_err_to_name(state.diagnostics.mount_error));
    }
}

void run_storage_test(AppState &state)
{
    const esp_err_t result =
        sticky_sdcard_run_storage_test(state.storage_test);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Storage test failed: %s (%s)",
                 state.storage_test.failure_reason,
                 esp_err_to_name(result));
    }
    update_sd_card_info(state);
}

void run_clear_card(AppState &state)
{
    const esp_err_t result =
        sticky_sdcard_clear_contents(state.clear_card);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Clear Card failed: %s (%s)",
                 state.clear_card.failure_reason,
                 esp_err_to_name(result));
    }
    update_sd_card_info(state);
}

void prepare_initialize_card(AppState &state)
{
    const esp_err_t result =
        sticky_sdcard_probe_initialize_target(state.initialize_card);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Initialize target probe failed: %s",
                 esp_err_to_name(result));
    }
}

void run_initialize_card(AppState &state)
{
    const esp_err_t result =
        sticky_sdcard_initialize_card(state.initialize_card);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "Initialize Card failed at %s (%s)",
                 state.initialize_card.failure_stage,
                 esp_err_to_name(result));
    }
    update_sd_card_info(state);
}
