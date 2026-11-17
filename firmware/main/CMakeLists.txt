set(VENDOR "vendor")
idf_component_register(
  SRCS "main.c" "telemetry.c" "control.c" "update.c" "update_manifest.c" "pages.c" "dashboard.c" "battery.c" "battery_model.c" "display_port.c" "font_rpm48.c" "font_text21.c" "font_unit22.c" "font_digits42.c" "font_digits66.c"
       "${VENDOR}/EXIO/TCA9554PWR.c" "${VENDOR}/Buzzer/Buzzer.c"
       "${VENDOR}/I2C_Driver/I2C_Driver.c" "${VENDOR}/LCD_Driver/ST7701S.c"
       "${VENDOR}/Touch_Driver/GT911.c"
       "${VENDOR}/Touch_Driver/esp_lcd_touch/esp_lcd_touch.c"
  INCLUDE_DIRS "." "${VENDOR}/EXIO" "${VENDOR}/Buzzer" "${VENDOR}/I2C_Driver"
       "${VENDOR}/LCD_Driver" "${VENDOR}/Touch_Driver"
       "${VENDOR}/Touch_Driver/esp_lcd_touch" "${VENDOR}/LVGL_Driver"
  REQUIRES driver esp_lcd esp_timer esp_wifi esp_netif esp_hw_support nvs_flash lwip json esp_adc mbedtls lvgl__lvgl app_update)
# The vendor drivers still use some deprecated IDF APIs. Keep those as warnings
# only; type errors and implicit declarations in our own code stay fatal.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=deprecated-declarations)
