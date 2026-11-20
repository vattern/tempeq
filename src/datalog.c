#include "datalog.h"

#include <string.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rtc_io.h"
#include "soc/rtc.h"
#include "ulp_lp_core.h"
#include "ulp_lp_core_lp_timer_shared.h"
#include "ulp_main.h"

static const char *TAG = "datalog";

extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_main_bin_end[] asm("_binary_ulp_main_bin_end");

#define LP_LOG ((volatile th_record_t *)&ulp_log_buf)
#define INTERVAL_US ((int64_t)TH_LOG_INTERVAL_S * 1000000LL)

// Wall-clock time we last programmed the LP timer for (0 = never).
// After that reading, the LP core falls back to its fixed 15-minute period.
RTC_DATA_ATTR static uint32_t scheduled_epoch;

void datalog_start_lp_core(void) {
  // The LP core bit-bangs I2C: open-drain LP GPIOs, released (high) at rest.
  const gpio_num_t pins[] = {TH_I2C_SDA_GPIO, TH_I2C_SCL_GPIO};
  for (int i = 0; i < 2; i++) {
    ESP_ERROR_CHECK(rtc_gpio_init(pins[i]));
    ESP_ERROR_CHECK(rtc_gpio_set_direction(pins[i], RTC_GPIO_MODE_INPUT_OUTPUT_OD));
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(pins[i]));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(pins[i]));
    ESP_ERROR_CHECK(rtc_gpio_set_level(pins[i], 1));
  }

  ESP_ERROR_CHECK(ulp_lp_core_load_binary(ulp_main_bin_start, ulp_main_bin_end - ulp_main_bin_start));
  ulp_wake_every_n = BLE_WAKE_EVERY_N_READINGS;

  ulp_lp_core_cfg_t cfg = {
      // HP_CPU: take the first reading immediately; LP_TIMER: then every interval.
      .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU | ULP_LP_CORE_WAKEUP_SOURCE_LP_TIMER,
      .lp_timer_sleep_duration_us = (uint64_t)INTERVAL_US,
  };
  scheduled_epoch = 0;
  ESP_ERROR_CHECK(ulp_lp_core_run(&cfg));
  ESP_LOGI(TAG, "LP core started");

  // Wait for the first reading, then (if the clock survived the reset) snap the
  // schedule to the wall-clock slots. Must happen after the LP core has finished
  // its run, or it would overwrite the timer with its default period.
  for (int i = 0; i < 100 && ulp_log_count == 0; i++) vTaskDelay(pdMS_TO_TICKS(10));
  vTaskDelay(pdMS_TO_TICKS(10));
  datalog_realign_after_reading();
}

void datalog_log_status(void) {
  ESP_LOGI(TAG, "%lu readings, last sensor status %lu (0 = ok)",
           (unsigned long)ulp_log_count, (unsigned long)ulp_last_error);
}

static int64_t now_us(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

uint32_t datalog_now(void) { return (uint32_t)(now_us() / 1000000LL); }

bool datalog_time_valid(void) { return datalog_now() >= MIN_VALID_EPOCH; }

uint32_t datalog_next_due(const reading_t *latest) {
  uint32_t now = datalog_now();
  if (scheduled_epoch > now) return scheduled_epoch;
  return latest ? latest->ts + TH_LOG_INTERVAL_S : now + TH_LOG_INTERVAL_S;
}

uint16_t datalog_snapshot(reading_t out[TH_LOG_CAPACITY]) {
  static th_record_t raw[TH_LOG_CAPACITY];
  uint32_t head, count, seq;

  // Seqlock: retry if the LP core wrote a record while we were copying.
  do {
    while ((seq = ulp_log_seq) & 1) {}
    head = ulp_log_head;
    count = ulp_log_count;
    for (int i = 0; i < TH_LOG_CAPACITY; i++) raw[i] = LP_LOG[i];
  } while (seq != ulp_log_seq);

  if (count > TH_LOG_CAPACITY) count = TH_LOG_CAPACITY;

  // Convert LP-timer ticks to wall-clock time using the current RTC calibration.
  uint64_t ticks_now = rtc_time_get();
  int64_t t_now = now_us();
  uint32_t cal = esp_clk_slowclk_cal_get();

  for (uint32_t i = 0; i < count; i++) {
    const th_record_t *r = &raw[(head + TH_LOG_CAPACITY - count + i) % TH_LOG_CAPACITY];
    uint64_t ticks = ((uint64_t)r->ticks_hi << 32) | r->ticks_lo;
    uint64_t age_us = ticks_now > ticks ? rtc_time_slowclk_to_us(ticks_now - ticks, cal) : 0;
    out[i].ts = (uint32_t)((t_now - (int64_t)age_us) / 1000000LL);
    out[i].temp = r->temp;
    out[i].hum = r->hum;
  }
  return (uint16_t)count;
}

// Program the LP timer so the next reading happens at `next_us` (wall clock).
static void schedule_next_reading(int64_t next_us) {
  int64_t delay = next_us - now_us();
  if (delay < 1000000) delay += INTERVAL_US;
  ulp_lp_core_lp_timer_set_wakeup_time((uint64_t)delay);
  scheduled_epoch = (uint32_t)((now_us() + delay) / 1000000LL);
  ESP_LOGI(TAG, "Next reading in %lld s", delay / 1000000LL);
}

void datalog_set_time(uint32_t epoch) {
  struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
  settimeofday(&tv, NULL);
  scheduled_epoch = 0;
  ESP_LOGI(TAG, "Clock set to %lu", (unsigned long)epoch);
  if (epoch >= MIN_VALID_EPOCH) {
    int64_t t = now_us();
    schedule_next_reading((t / INTERVAL_US + 1) * INTERVAL_US);
  }
}

void datalog_realign_after_reading(void) {
  if (!datalog_time_valid()) return;
  // The reading just taken belongs to the slot nearest "now"; aim for the one after it.
  int64_t t = now_us();
  schedule_next_reading(((t + INTERVAL_US / 2) / INTERVAL_US + 1) * INTERVAL_US);
}
