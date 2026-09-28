# Project-local override of ESP-IDF's FatFs component.
#
# Keep using the sources shipped with the selected ESP-IDF installation.  The
# behavioral differences stay limited to the exFAT configuration, the detailed
# mount error log, and one dedicated Partition-1 mapping used by Initialize.
# An IDF update therefore cannot silently mix a copied source tree with another
# IDF, and both generated-source patches fail configuration if their anchors
# change.
# Normalize the environment value first because Windows may provide IDF_PATH
# with backslashes, which are escape characters in generated CMake/C sources.
file(TO_CMAKE_PATH "$ENV{IDF_PATH}" IDF_PATH_CMAKE)
set(IDF_FATFS_DIR "${IDF_PATH_CMAKE}/components/fatfs")
message(STATUS
    "Using project-local FatFs override: exFAT=1, LBA64=0, TRIM=0")

idf_build_get_property(target IDF_TARGET)

# Keep normal volumes on ESP-IDF's auto-detect mapping, but reserve logical
# drive 1 for Initialize Card so f_mkfs formats the already-created first MBR
# partition instead of replacing the MBR with FatFs' legacy LBA-63 layout.
set(diskio_source "${IDF_FATFS_DIR}/diskio/diskio.c")
set(diskio_with_initialize_partition
    "${CMAKE_CURRENT_BINARY_DIR}/diskio_with_initialize_partition.c")
file(READ "${diskio_source}" diskio_contents)
set(diskio_partition_map_original
    "{1, 0},    /* Logical drive 1 ==> Physical drive 1, auto detection */")
set(diskio_partition_map_replacement
    "{1, 1},    /* Logical drive 1 ==> Physical drive 1, MBR partition 1 */")
string(FIND "${diskio_contents}"
       "${diskio_partition_map_original}"
       diskio_partition_map_position)
if(diskio_partition_map_position EQUAL -1)
    message(FATAL_ERROR
        "ESP-IDF FatFs drive mapping changed; update Initialize partition mapping")
endif()
string(REPLACE
    "${diskio_partition_map_original}"
    "${diskio_partition_map_replacement}"
    diskio_contents
    "${diskio_contents}")
file(WRITE "${diskio_with_initialize_partition}" "${diskio_contents}")

set(srcs
    "${diskio_with_initialize_partition}"
    "${IDF_FATFS_DIR}/diskio/diskio_rawflash.c"
    "${IDF_FATFS_DIR}/diskio/diskio_wl.c"
    "${IDF_FATFS_DIR}/src/ff.c"
    "${IDF_FATFS_DIR}/src/ffunicode.c")

set(include_dirs
    "${IDF_FATFS_DIR}/diskio"
    "${IDF_FATFS_DIR}/src")

set(requires "wear_levelling")

# Keep the ESP-IDF implementation intact and generate a build-local copy with
# one diagnostic-only log improvement.  The stock helper collapses f_mount()
# failures to ESP_FAIL; this preserves the numeric FatFs FRESULT in the log.
set(vfs_sdmmc_source
    "${IDF_FATFS_DIR}/vfs/vfs_fat_sdmmc.c")
set(vfs_sdmmc_with_fresult
    "${CMAKE_CURRENT_BINARY_DIR}/vfs_fat_sdmmc_with_fresult.c")
file(READ "${vfs_sdmmc_source}" vfs_sdmmc_contents)
set(vfs_mount_log_original
    "ESP_LOGW(TAG, \"failed to mount card (%d)\", res);")
set(vfs_mount_log_replacement
    "ESP_LOGE(TAG, \"FatFs f_mount failed: FRESULT=%d\", res);")
string(FIND "${vfs_sdmmc_contents}"
       "${vfs_mount_log_original}" vfs_mount_log_position)
if(vfs_mount_log_position EQUAL -1)
    message(FATAL_ERROR
        "ESP-IDF FatFs mount log changed; update the local exFAT override")
endif()
string(REPLACE
    "${vfs_mount_log_original}"
    "${vfs_mount_log_replacement}"
    vfs_sdmmc_contents
    "${vfs_sdmmc_contents}")
file(WRITE "${vfs_sdmmc_with_fresult}" "${vfs_sdmmc_contents}")

if(${target} STREQUAL "linux")
    list(APPEND srcs "${IDF_FATFS_DIR}/port/linux/ffsystem.c")
else()
    list(APPEND srcs
        "${IDF_FATFS_DIR}/port/freertos/ffsystem.c"
        "${IDF_FATFS_DIR}/diskio/diskio_sdmmc.c"
        "${IDF_FATFS_DIR}/vfs/vfs_fat.c"
        "${vfs_sdmmc_with_fresult}"
        "${IDF_FATFS_DIR}/vfs/vfs_fat_spiflash.c")
    list(APPEND include_dirs "${IDF_FATFS_DIR}/vfs")
    list(APPEND requires "sdmmc" "esp_driver_sdmmc" "esp_driver_sdspi")
    list(APPEND priv_requires "vfs" "esp_driver_gpio")
endif()

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/ffconf_exfat_override.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ffconf_exfat_override.h"
    @ONLY)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${requires}
    PRIV_REQUIRES ${priv_requires})

# Apply the configuration only while compiling the FatFs component.  A PUBLIC
# forced include reaches C++ users before ff.h opens its extern "C" block and
# gives ff_memalloc/ff_memfree C++ linkage.  The application only uses the
# stable diskio and mkfs types, so it must include the normal public headers
# without this private implementation configuration being injected first.
target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE "-include${CMAKE_CURRENT_BINARY_DIR}/ffconf_exfat_override.h")
