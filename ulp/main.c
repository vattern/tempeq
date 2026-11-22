// LP-core program: runs every TH_LOG_INTERVAL_S (woken by the LP timer),
// reads the AHT21 over bit-banged I2C, appends to a staging ring buffer in LP
// SRAM and wakes the main core when it is due a BLE window or the ring needs
// emptying into flash. Runs for ~100 ms per reading; the main core
// and radio stay in deep sleep throughout.
//
// Every global here is visible to the HP core as `ulp_<name>`.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ulp_lp_core.h"
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_utils.h"
#include "ulp_lp_core_lp_timer_shared.h"
#include "../include/th_shared.h"

#define AHT_ADDR      0x38

// ---- Shared with the HP core ----------------------------------------------
volatile th_record_t log_buf[TH_LP_RING_CAPACITY];
volatile uint32_t log_total;     // readings ever taken; record i is at log_buf[i % capacity]
volatile uint32_t flushed_total; // set by HP: readings already moved to flash
volatile uint32_t log_seq;       // seqlock: odd while a record is being written
volatile uint32_t wake_every_n;  // set by HP: wake main core every N readings (0 = never)
volatile uint32_t wake_reason;   // TH_WAKE_* for the last wake we triggered
volatile uint32_t readings_since_wake;
volatile uint32_t sensor_powered_up;
// Diagnostics: 0 ok, 1 no ACK to status read (not connected / wiring / pull-ups),
// 2 no ACK to measure command, 3 read failed, 4 still busy, 5 CRC mismatch.
volatile uint32_t last_error;

// ---- Bit-banged I2C ---------------------------------------------------------
// The HP core configures both pins as open-drain LP GPIOs with pull-ups, so
// writing 1 releases the line and writing 0 pulls it low. ~50-100 kHz.
#define SDA ((lp_io_num_t)TH_I2C_SDA_GPIO)
#define SCL ((lp_io_num_t)TH_I2C_SCL_GPIO)
#define HALF_BIT() ulp_lp_core_delay_us(5)

static void scl_release(void) {
  ulp_lp_core_gpio_set_level(SCL, 1);
  for (int i = 0; i < 1000 && !ulp_lp_core_gpio_get_level(SCL); i++) HALF_BIT();  // clock stretching
}

static void i2c_start(void) {
  ulp_lp_core_gpio_set_level(SDA, 1);
  scl_release();
  HALF_BIT();
  ulp_lp_core_gpio_set_level(SDA, 0);
  HALF_BIT();
  ulp_lp_core_gpio_set_level(SCL, 0);
}

static void i2c_stop(void) {
  ulp_lp_core_gpio_set_level(SDA, 0);
  HALF_BIT();
  scl_release();
  HALF_BIT();
  ulp_lp_core_gpio_set_level(SDA, 1);
  HALF_BIT();
}

static bool i2c_write_byte(uint8_t b) {  // returns true on ACK
  for (int i = 7; i >= 0; i--) {
    ulp_lp_core_gpio_set_level(SDA, (b >> i) & 1);
    HALF_BIT();
    scl_release();
    HALF_BIT();
    ulp_lp_core_gpio_set_level(SCL, 0);
  }
  ulp_lp_core_gpio_set_level(SDA, 1);
  HALF_BIT();
  scl_release();
  bool ack = !ulp_lp_core_gpio_get_level(SDA);
  HALF_BIT();
  ulp_lp_core_gpio_set_level(SCL, 0);
  return ack;
}

static uint8_t i2c_read_byte(bool ack) {
  uint8_t b = 0;
  ulp_lp_core_gpio_set_level(SDA, 1);
  for (int i = 0; i < 8; i++) {
    HALF_BIT();
    scl_release();
    b = (b << 1) | (ulp_lp_core_gpio_get_level(SDA) ? 1 : 0);
    HALF_BIT();
    ulp_lp_core_gpio_set_level(SCL, 0);
  }
  ulp_lp_core_gpio_set_level(SDA, ack ? 0 : 1);
  HALF_BIT();
  scl_release();
  HALF_BIT();
  ulp_lp_core_gpio_set_level(SCL, 0);
  ulp_lp_core_gpio_set_level(SDA, 1);
  return b;
}

static bool i2c_write(const uint8_t *buf, size_t len) {
  i2c_start();
  bool ok = i2c_write_byte(AHT_ADDR << 1);
  for (size_t i = 0; ok && i < len; i++) ok = i2c_write_byte(buf[i]);
  i2c_stop();
  return ok;
}

static bool i2c_read(uint8_t *buf, size_t len) {
  i2c_start();
  bool ok = i2c_write_byte((AHT_ADDR << 1) | 1);
  for (size_t i = 0; ok && i < len; i++) buf[i] = i2c_read_byte(i + 1 < len);
  i2c_stop();
  return ok;
}

// ---- AHT21 ------------------------------------------------------------------
static bool aht_write3(uint8_t a, uint8_t b, uint8_t c) {
  uint8_t buf[3] = {a, b, c};
  return i2c_write(buf, 3);
}

static bool aht_read_bytes(uint8_t *buf, size_t len) { return i2c_read(buf, len); }

// Register re-initialisation from the AHT21 datasheet, needed when status
// bits 3 and 4 are not both set.
static void aht_reset_reg(uint8_t reg) {
  uint8_t buf[3] = {0};
  aht_write3(reg, 0x00, 0x00);
  ulp_lp_core_delay_us(5000);
  aht_read_bytes(buf, 3);
  ulp_lp_core_delay_us(10000);
  aht_write3(0xB0 | reg, buf[1], buf[2]);
}

static uint8_t crc8(const uint8_t *data, int len) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

static bool aht_measure(int16_t *temp, uint16_t *hum) {
  if (!sensor_powered_up) {  // first run after power-on: AHT21 needs 100 ms
    ulp_lp_core_delay_us(100000);
    sensor_powered_up = 1;
  }

  uint8_t status;
  if (!aht_read_bytes(&status, 1)) { last_error = 1; return false; }
  if ((status & 0x18) != 0x18) {
    aht_reset_reg(0x1B);
    aht_reset_reg(0x1C);
    aht_reset_reg(0x1E);
    ulp_lp_core_delay_us(10000);
  }

  if (!aht_write3(0xAC, 0x33, 0x00)) { last_error = 2; return false; }
  ulp_lp_core_delay_us(80000);

  uint8_t d[7];
  int attempt = 0;
  for (;;) {
    if (!aht_read_bytes(d, 7)) { last_error = 3; return false; }
    if (!(d[0] & 0x80)) break;           // busy bit cleared
    if (++attempt >= 5) { last_error = 4; return false; }
    ulp_lp_core_delay_us(10000);
  }
  if (crc8(d, 6) != d[6]) { last_error = 5; return false; }
  last_error = 0;

  uint32_t raw_h = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
  uint32_t raw_t = (((uint32_t)d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];
  // Integer maths (no FPU): centi-%RH = raw * 10000 / 2^20, centi-degC = raw * 20000 / 2^20 - 5000
  *hum = (uint16_t)((raw_h * 625u) >> 16);
  *temp = (int16_t)((int32_t)((raw_t * 625u) >> 15) - 5000);
  return true;
}

// ---------------------------------------------------------------------------
int main(void) {
  uint64_t ticks = ulp_lp_core_lp_timer_get_cycle_count();

  int16_t temp = TH_TEMP_ERROR;
  uint16_t hum = TH_HUM_ERROR;
  if (!aht_measure(&temp, &hum)) {
    temp = TH_TEMP_ERROR;
    hum = TH_HUM_ERROR;
  }

  uint32_t total = log_total;
  uint32_t head = total % TH_LP_RING_CAPACITY;
  log_seq++;
  log_buf[head].ticks_lo = (uint32_t)ticks;
  log_buf[head].ticks_hi = (uint32_t)(ticks >> 32);
  log_buf[head].temp = temp;
  log_buf[head].hum = hum;
  log_total = total + 1;
  log_seq++;

  if (wake_every_n && ++readings_since_wake >= wake_every_n) {
    readings_since_wake = 0;
    wake_reason = TH_WAKE_BLE;
    ulp_lp_core_wakeup_main_processor();
  } else if (log_total - flushed_total >= TH_LP_RING_CAPACITY - TH_LP_FLUSH_MARGIN) {
    wake_reason = TH_WAKE_FLUSH;
    ulp_lp_core_wakeup_main_processor();
  }
  return 0;  // LP core halts; the LP timer wakes it again next interval
}
