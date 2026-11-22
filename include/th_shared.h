// Definitions shared between the LP-core program (ulp/) and the HP firmware (src/).
#pragma once
#include <stdint.h>

#define TH_LOG_INTERVAL_S   (10 * 60)                          // 10 minutes

// The LP core stages readings in a ring buffer in LP SRAM. The main core moves
// them to the flash log every time it wakes. The ring only has to cover the gap
// between wakes: when it is within TH_LP_FLUSH_MARGIN of full, the LP core wakes
// the main core just to move them (even if BLE wakes are disabled).
#define TH_LP_RING_CAPACITY (24 * 3600 / TH_LOG_INTERVAL_S)    // 144 = 24 hours
#define TH_LP_FLUSH_MARGIN  6

// Why the LP core woke the main core (ulp_wake_reason).
#define TH_WAKE_BLE         1   // regular BLE window
#define TH_WAKE_FLUSH       2   // ring nearly full: move readings, no BLE

// AHT21 I2C pins on the XIAO ESP32-C6 headers. The LP core bit-bangs I2C, so
// these must be LP GPIOs (GPIO0..7); the XIAO only breaks out GPIO0..2 (D0..D2).
// The C6's hardware LP-I2C is fixed to GPIO6/7, which the XIAO only exposes on
// the tiny JTAG pads underneath (MTCK/MTDO).
#define TH_I2C_SDA_GPIO     2   // XIAO D2
#define TH_I2C_SCL_GPIO     1   // XIAO D1

#define TH_TEMP_ERROR       INT16_MIN   // stored when the sensor read fails
#define TH_HUM_ERROR        0xFFFF

// One reading, written by the LP core into LP SRAM.
// `ticks` is the LP-timer (RTC) counter at the time of the reading; the HP
// core converts it to unix time when serving data, so setting the clock
// automatically corrects every stored timestamp.
typedef struct {
  uint32_t ticks_lo;
  uint32_t ticks_hi;
  int16_t  temp;   // centi-degC
  uint16_t hum;    // centi-%RH
} th_record_t;
