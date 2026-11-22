#pragma once

#include "th_shared.h"

// Info-level logging (costs awake time). Use the esp32c6-debug env.
#ifndef TH_DEBUG
#define TH_DEBUG 0
#endif

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------
// Board: Seeed XIAO ESP32-C6.
// AHT21 I2C pins are in th_shared.h (SDA = D2/GPIO2, SCL = D1/GPIO1).

// Optional push-button (to GND) that wakes the main core and opens a long BLE
// window. Must be an LP GPIO; on the XIAO that leaves D0 (GPIO0).
// Fit an external pull-up (100k to 3V3). Set to -1 to disable.
#define PIN_WAKE_BUTTON    0

// XIAO ESP32-C6 RF switch: GPIO3 LOW powers it, GPIO14 selects the antenna
// (LOW = on-board ceramic, HIGH = U.FL connector).
#define PIN_RF_SWITCH_EN   3
#define PIN_RF_ANT_SELECT  14
#define USE_EXTERNAL_ANTENNA 0

// ---------------------------------------------------------------------------
// BLE availability (the dominant power cost - tune for your battery)
// ---------------------------------------------------------------------------
// The LP core wakes the main core after every N readings to advertise for
// BLE_WINDOW_AFTER_WAKE_MS (and move readings to flash). 6 = once an hour.
// 0 = never (button only, lowest power; readings still reach flash daily).
#define BLE_WAKE_EVERY_N_READINGS     6
#define BLE_WINDOW_AFTER_WAKE_MS      5000
// Advertise for this long after a button press or power-on.
#define BLE_WINDOW_BUTTON_MS          60000
// Go to sleep once a connected client has made no requests for this long.
#define BLE_MAX_IDLE_MS               30000
// Advertising interval (units of 0.625 ms). 160 = 100 ms.
#define BLE_ADV_INTERVAL              160

#define BLE_DEVICE_NAME    "TH-Logger"

// Records per history page. 9 + 60 * 8 = 489 bytes (< 512 byte ATT limit).
#define HISTORY_PAGE_SIZE  60

// Any unix time before this is considered "clock not set" (2020-01-01).
#define MIN_VALID_EPOCH    1577836800UL
