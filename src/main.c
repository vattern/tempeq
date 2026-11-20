// Ultra-low-power Seeed XIAO ESP32-C6 + AHT21 temperature/humidity logger.
//
// The LP core (ulp/main.c) does all the logging: every 15 minutes the LP timer
// wakes it, it reads the AHT21 over bit-banged I2C and stores the result in LP SRAM.
// This main core only wakes to serve BLE (and USB):
//   - after a button press (long window),
//   - when the LP core wakes it every BLE_WAKE_EVERY_N_READINGS readings,
//   - on power-on (to start the LP core and give a window to set the clock).
// If a USB host is attached during that window, it stays awake (and keeps
// advertising) until the cable is unplugged, serving commands on the USB
// serial port (see usb_cmd.h).
// The log survives deep sleep but is lost on power loss / hard reset.

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "ble.h"
#include "config.h"
#include "datalog.h"
#include "usb_cmd.h"

static const char *TAG = "main";

// The XIAO's RF switch must be powered and pointed at an antenna for BLE.
static void rf_switch(bool on) {
  if (on) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_RF_SWITCH_EN) | (1ULL << PIN_RF_ANT_SELECT),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(PIN_RF_SWITCH_EN, 0);
    gpio_set_level(PIN_RF_ANT_SELECT, USE_EXTERNAL_ANTENNA);
  } else {
    // Switch off and hold it off through deep sleep.
    gpio_set_direction(PIN_RF_SWITCH_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_RF_SWITCH_EN, 1);
    gpio_hold_en(PIN_RF_SWITCH_EN);
  }
}

static void go_to_sleep(void) {
  rf_switch(false);
  ESP_ERROR_CHECK(esp_sleep_enable_ulp_wakeup());
#if PIN_WAKE_BUTTON >= 0
  // Only arm the button if it is released, otherwise we'd wake immediately.
  if (gpio_get_level(PIN_WAKE_BUTTON) == 1)
    ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_WAKE_BUTTON, ESP_EXT1_WAKEUP_ANY_LOW));
#endif
  ESP_LOGI(TAG, "Deep sleep");
  esp_deep_sleep_start();
}

// Serve BLE for `window_ms`, longer while a BLE client or USB host is attached.
static void serve(uint32_t window_ms) {
  ble_start();
  usb_cmd_start();
  ESP_LOGI(TAG, "Serving for %lu ms", (unsigned long)window_ms);

  int64_t window_end = esp_timer_get_time() + (int64_t)window_ms * 1000;
  bool usb_was_connected = false;
  for (;;) {
    bool usb = usb_cmd_host_connected();
    if (usb != usb_was_connected) {
      ESP_LOGI(TAG, "USB host %s", usb ? "connected: staying awake" : "disconnected");
      usb_was_connected = usb;
    }
    if (usb) usb_cmd_poll();

    bool window_open = esp_timer_get_time() < window_end;
    int64_t ble_conn = ble_connected_for_us();
    bool ble_busy = ble_conn >= 0 && ble_conn < (int64_t)BLE_MAX_CONNECTED_MS * 1000;
    if (!window_open && !usb && !ble_busy) break;

    ble_set_advertising(window_open || usb);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  ble_stop();
}

void app_main(void) {
  esp_log_level_set("*", TH_DEBUG ? ESP_LOG_INFO : ESP_LOG_WARN);

#if PIN_WAKE_BUTTON >= 0
  gpio_config_t btn = {
      .pin_bit_mask = 1ULL << PIN_WAKE_BUTTON,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&btn);
#endif

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool cold_boot = esp_reset_reason() != ESP_RST_DEEPSLEEP;
  ESP_LOGI(TAG, "Wake cause %d, cold boot %d", cause, cold_boot);

  uint32_t window = BLE_WINDOW_BUTTON_MS;
  if (cold_boot) {
    datalog_start_lp_core();
  } else if (cause == ESP_SLEEP_WAKEUP_ULP) {
    datalog_realign_after_reading();
    window = BLE_WINDOW_AFTER_WAKE_MS;
  }

  datalog_log_status();

  if (window > 0) {
    // NVS holds the RF calibration; without it every wake does a full (slower) calibration.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      nvs_flash_erase();
      nvs_flash_init();
    }
    gpio_hold_dis(PIN_RF_SWITCH_EN);
    rf_switch(true);
    serve(window);
  }
  go_to_sleep();
}
