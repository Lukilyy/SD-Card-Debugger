# Sticky SD Card Debugger

<div align="center">
  <img src="page.jpg" alt="reTerminal Sticky SD Card Debugger" width="425">
</div>

An ESP-IDF SD card information, file-management, and diagnostic utility for
the Seeed Studio reTerminal Sticky. It supports FAT32 and exFAT cards in the
target capacity range of 4 GB to 256 GB.

## Build and flash (ESP-IDF 5.4)

Use a reTerminal Sticky with ESP32-S3 and an ESP-IDF **v5.4.0** command
environment. From the repository root, run:

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p COM5 flash monitor
```

Replace `COM5` with your board's serial port; on Linux/macOS use the detected
`/dev/...` device instead. The first build may download
the `espressif/cmake_utilities` dependency declared in `main/idf_component.yml`;
an internet connection is needed for that download. `sdkconfig.defaults`
contains the target, 32 MB flash, PSRAM, and FatFs defaults. The project-local
`components/fatfs` override enables exFAT using the installed ESP-IDF 5.4
sources; it does not edit the ESP-IDF installation. Build output, generated
`sdkconfig`, and downloaded `managed_components` are local files and are
excluded from Git.

Flashing replaces firmware on the connected device. `monitor` prints the
startup SD Card Report and subsequent diagnostic logs.

## SD Card Info home page

- Card presence, raw access, and filesystem mount status
- Automatic updates after card insertion or removal
- Total, used, and free capacity
- FAT32 or exFAT filesystem identification
- MBR, GPT, partition, sector, and cluster information when available
- Global `GB / GiB` switch for decimal (KB/MB/GB) or binary (KiB/MiB/GiB)
  capacity display
- Useful raw card information remains available when filesystem mounting fails
- `HELP`, `FILES`, and `TOOLS` entries

Press AI/OK on the home page to refresh the card information manually.

## Files

- Browse directories with long filename (LFN) support and pagination
- Enter folders and return to the parent directory using buttons or touch
- View file and folder properties, including path and FAT modification time
- Create folders and empty TXT, JSON, CSV, MD, or BIN files
- Delete individual files
- Delete empty or non-empty folders recursively after confirmation
- Safe name validation, duplicate-name checks, and SD removal handling
- RTC-backed FAT timestamps; an on-screen date/time setup page is shown before
  creation if the PCF8563 RTC is invalid

Rename and file-content editing are not implemented.

For normal use, insert a card and wait for the home page to update. Tap
`FILES` to browse; use UP/DOWN to select, AI/OK or touch to open, the top-right
return arrow to go back, and the bottom arrows to change pages. The `...`
entry opens File or Folder Properties. Tap `NEW` to create an entry. Swipe up
from Files to return to SD Info.

## Tools

### Diagnostics

Read-only raw diagnostics inspect the card even when FAT32/exFAT mounting
fails. Summary and detail pages report the card state, raw card information,
MBR/GPT structures, partitions, boot sectors, filesystems, and mount result.
GPT headers, CRC status, and valid partition entries are reported when present.
Diagnostics never repairs, formats, or writes raw sectors.

### Storage Test

The user-started storage test creates a dedicated temporary file, writes test
data, reads it back, verifies the content, and removes the file. Write, read,
verify, and cleanup results are shown separately. Existing user files are not
overwritten.

### Clear Card

After explicit confirmation, Clear Card recursively removes all files and
folders from a mountable card. It preserves the existing partition table and
filesystem format; it does not format the card or write raw partition sectors.

### Initialize Card

After explicit confirmation, Initialize Card erases and rebuilds the entire
card using one MBR primary partition:

| Card capacity | Result |
| --- | --- |
| 4 GB–32 GB | MBR + FAT32, 32 KiB cluster |
| Greater than 32 GB–256 GB | MBR + exFAT, FatFs-selected allocation unit |

The partition starts at LBA 2048 for 1 MiB alignment. Initialization formats
the existing partition, then verifies the MBR, filesystem boot data, and an
actual filesystem mount. It does not automatically run Storage Test.

Use Diagnostics first when a card cannot mount. Storage Test writes and then
removes a temporary file. Clear Card removes all files and folders while
keeping the current partition and filesystem. Initialize Card erases the
existing layout and data; back up the card before confirming it.

Both FAT32 and exFAT initialization have been verified across a Sticky reboot,
Windows recognition, and actual file read/write operations.

## Help and device status

The two-part Help page describes the debugger and explains when to use Files,
Diagnostics, Storage Test, Clear Card, and Initialize Card, including which
operations modify data. It also shows the latest BQ27220 battery percentage and
charging state. Long-press AI/OK to enter deep sleep.

At startup, Serial Monitor receives one detailed SD Card Report with reliable
card identity, capacity, sector, interface, partition, filesystem, mount, and
volume information. Battery status is logged at startup and every 60 seconds.

## Hardware implementation

The project reuses the validated Sticky drivers and board initialization:

| Function | Implementation |
| --- | --- |
| Display | SSD1677, 800 × 480 logical Canvas on SPI2; the driver rotates the framebuffer for the panel |
| UI | 1-bit black and white, partial refresh with periodic full refresh to clear ghosting |
| Touch | GT911 on I2C0, using the application event queue |
| Buttons | GPIO buttons through the existing `iot_button` component |
| MicroSD | SDSPI on the display's shared SPI2 bus |
| RTC | PCF8563 for FAT file and folder timestamps |
| Battery | BQ27220 state-of-charge plus GPIO charging detection |

MicroSD signals are SCK GPIO13, MOSI GPIO14, MISO GPIO12, CS GPIO8,
power-enable GPIO10 (active high), and card-detect GPIO11 (active low). The
display uses its own CS on GPIO15.

SD and the e-paper display share SPI2. Every workflow follows the established
sequence: acquire and initialize SD, perform the operation, unmount/release SD,
return SPI2 to the display, and only then refresh the e-paper. Raw Diagnostics
uses the same ownership path and does not create a competing SD/SPI driver.

## Metadata and safety

- Mount-independent Raw Diagnostics supplies card, partition, and filesystem
  information when normal VFS mounting is unavailable.
- Capacity and file-size values retain their original byte/sector data; only
  display formatting changes with the GB/GiB setting.
- Unknown or invalid structures are reported rather than guessed.
- Diagnostics is strictly read-only.
- Storage Test uses a collision-safe temporary file and attempts cleanup on
  every exit path.
- Clear Card and Initialize Card require explicit confirmation.
- Clear Card preserves the current partition table and filesystem.
- Initialize Card is destructive and rebuilds the partition and filesystem.

## Code structure

- `main/main.cpp`, `main/board/`: startup and Sticky board initialization
- `main/app/app.*`, `app_event.*`, `touch_input.*`: application events,
  navigation, touch dispatch, and e-paper refresh policy
- `main/app/app_data.*`: SD Info, Diagnostics, Tools, and startup Report state
- `main/app/app_file_data.*`: Files, create-entry, and RTC input state
- `main/pages/`: SD Info, Files, Properties, creation, RTC setup, Help, and
  Tools rendering; `main/ui/` contains Canvas, the built-in font, capacity
  formatting, and shared page controls
- `main/devices/sticky_sdcard.*`: SD detection, shared SPI2 lifecycle, mount,
  raw diagnostics, file operations, Storage Test, Clear Card, and Initialize
- `main/devices/sticky_sdcard_report.cpp`: formatting the collected SD Report
- `main/devices/`: display, touch, buttons, RTC, battery, and power support
- `components/`: local FatFs override and required device/button components;
  `dependencies.lock` pins the managed build-helper dependency

## Verification status

The completed FAT32 and exFAT workflows have been verified on reTerminal
Sticky hardware. This includes hot-plug detection, browsing and file
operations, diagnostics, Storage Test, Clear Card, initialization, rebooting
the Sticky with an initialized card, Windows recognition, and actual file
read/write access.

The 4 GB to 256 GB range is a design target, not a claim that every card model
and capacity has been tested. The project's original code is provided under
the root [MIT License](LICENSE). The bundled Espressif button component retains
its [Apache 2.0 license](components/button/license.txt).
