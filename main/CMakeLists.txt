idf_component_register(
    SRCS
         # Application state and event handling
         "main.cpp"
         "app/app.cpp"
         "app/app_battery.cpp"
         "app/app_data.cpp"
         "app/app_file_data.cpp"
         "app/app_event.cpp"
         "app/touch_input.cpp"
         "board/board.cpp"

         # Display and shared UI
         "devices/sticky_display.cpp"
         "ui/canvas.cpp"
         "ui/capacity_format.cpp"
         "ui/font.cpp"

         # Pages
         "pages/home_page.cpp"
         "pages/file_page.cpp"
         "pages/file_detail_page.cpp"
         "pages/new_entry_page.cpp"
         "pages/rtc_setup_page.cpp"
         "pages/help_page.cpp"
         "pages/tools_page.cpp"

         # Input, battery, and feedback
         "devices/sticky_buttons.cpp"
         "devices/sticky_buzzer.cpp"
         "devices/sticky_battery.cpp"
         "devices/sticky_charger.cpp"
         "devices/sticky_touch.cpp"

         # SD access, report formatting, and RTC
         "devices/sticky_sdcard.cpp"
         "devices/sticky_sdcard_report.cpp"
         "devices/sticky_rtc.cpp"

         # Deep sleep
         "devices/sticky_power.cpp"
         "pages/sleep_page.cpp"
    INCLUDE_DIRS "." "app" "board" "devices" "pages" "ui"
    REQUIRES esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_spi esp_hw_support esp_timer bq27220 button fatfs gt911 sdmmc seeed_epaper
)
