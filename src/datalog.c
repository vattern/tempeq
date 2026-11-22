#include "datalog.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_private/esp_clk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/rtc_io.h"
#include "nvs.h"
#include "soc/rtc.h"
#include "ulp_lp_core.h"
#include "ulp_lp_core_lp_timer_shared.h"
#include "ulp_main.h"

static const char *TAG = "datalog";

extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_main_bin_end[] asm("_binary_ulp_main_bin_end");

#define LP_RING ((volatile th_record_t *)&ulp_log_buf)
#define INTERVAL_US ((int64_t)TH_LOG_INTERVAL_S * 1000000LL)

// ---------------------------------------------------------------------------
// Flash log format
// ---------------------------------------------------------------------------
// A ring of 4 KB sectors in the "thlog" partition. Each sector starts with a
// header carrying a sequence number that increases by one per sector written,
// followed by fixed-size records appended in order. Erased flash (0xFF) marks
// unused slots. Record number = sector seq * RECS_PER_SECTOR + slot.
//
// `base` in the newest sector's header is the oldest sector seq still valid.
// Clearing the log just starts a new sector with base = its own seq.

#define SECTOR_SIZE 4096
#define LOG_PARTITION_SUBTYPE 0x40  // custom data subtype, see partitions.csv
#define HDR_MAGIC   0x314C4854u  // "THL1"

typedef struct {
  uint32_t magic;
  uint32_t seq;
  uint32_t base;
  uint32_t reserved;
} sector_hdr_t;

typedef struct {
  uint32_t ts;      // unix time, or seconds since power-on if the clock wasn't set
  uint16_t boot;    // power-on session the reading was taken in
  int16_t  temp;
  uint16_t hum;
  uint16_t reserved;
} flash_rec_t;

#define RECS_PER_SECTOR ((SECTOR_SIZE - sizeof(sector_hdr_t)) / sizeof(flash_rec_t))  // 340
#define EMPTY_TS 0xFFFFFFFFu

// Log position, kept across deep sleep so we only scan flash on a cold boot.
typedef struct {
  uint32_t magic;       // RTC_STATE_MAGIC when valid
  uint32_t head_sector; // physical index of the newest sector
  uint32_t head_seq;
  uint32_t head_used;   // records in the newest sector
  uint32_t base_seq;
  uint32_t boot;        // current power-on session
} log_state_t;
#define RTC_STATE_MAGIC 0x5354484Cu

RTC_DATA_ATTR static log_state_t st;

// Wall-clock time we last programmed the LP timer for (0 = never).
// After that reading, the LP core falls back to its fixed period.
RTC_DATA_ATTR static uint32_t scheduled_epoch;

static const esp_partition_t *part;
static uint32_t n_sectors;
static SemaphoreHandle_t lock;
static nvs_handle_t nvs;
static bool nvs_ok;

#define LOCK() xSemaphoreTake(lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(lock)

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------
static int64_t now_us(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

uint32_t datalog_now(void) { return (uint32_t)(now_us() / 1000000LL); }

bool datalog_time_valid(void) { return datalog_now() >= MIN_VALID_EPOCH; }

// ---------------------------------------------------------------------------
// Clock offsets for readings taken before the clock was set.
// NVS key "o<boot>" = unix time - seconds-since-power-on for that session.
// ---------------------------------------------------------------------------
static struct { uint16_t boot; bool found; int32_t off; } off_cache[4];
static int off_cache_next;

static void off_cache_reset(void) { memset(off_cache, 0xFF, sizeof(off_cache)); }

static bool boot_offset(uint16_t boot, int32_t *off) {
  for (int i = 0; i < 4; i++) {
    if (off_cache[i].boot == boot) {
      *off = off_cache[i].off;
      return off_cache[i].found;
    }
  }
  char key[16];
  snprintf(key, sizeof(key), "o%u", boot);
  int32_t v = 0;
  bool found = nvs_ok && nvs_get_i32(nvs, key, &v) == ESP_OK;
  off_cache[off_cache_next].boot = boot;
  off_cache[off_cache_next].found = found;
  off_cache[off_cache_next].off = v;
  off_cache_next = (off_cache_next + 1) % 4;
  *off = v;
  return found;
}

static uint32_t resolve_ts(const flash_rec_t *r) {
  if (r->ts >= MIN_VALID_EPOCH) return r->ts;
  int32_t off;
  if (boot_offset(r->boot, &off)) return r->ts + off;
  if (r->boot == (uint16_t)st.boot) return r->ts;  // client maps it via the device's current time
  return 0;                                         // earlier power-on, clock never set
}

// ---------------------------------------------------------------------------
// Flash access
// ---------------------------------------------------------------------------
static uint32_t sector_of_seq(uint32_t seq) {
  return (st.head_sector + n_sectors - (st.head_seq - seq) % n_sectors) % n_sectors;
}

static size_t rec_offset(uint32_t sector, uint32_t slot) {
  return (size_t)sector * SECTOR_SIZE + sizeof(sector_hdr_t) + slot * sizeof(flash_rec_t);
}

// Start a new sector after the newest one. `clear` drops everything before it.
static void new_sector(bool clear) {
  uint32_t sector = (st.head_sector + 1) % n_sectors;
  uint32_t seq = st.head_seq + 1;
  uint32_t base = clear ? seq : st.base_seq;
  if (seq - base >= n_sectors) base = seq - n_sectors + 1;  // overwrote the oldest
  ESP_ERROR_CHECK(esp_partition_erase_range(part, (size_t)sector * SECTOR_SIZE, SECTOR_SIZE));
  sector_hdr_t h = {.magic = HDR_MAGIC, .seq = seq, .base = base, .reserved = 0xFFFFFFFF};
  ESP_ERROR_CHECK(esp_partition_write(part, (size_t)sector * SECTOR_SIZE, &h, sizeof(h)));
  st.head_sector = sector;
  st.head_seq = seq;
  st.head_used = 0;
  st.base_seq = base;
}

static void append(const flash_rec_t *r) {
  if (st.head_used >= RECS_PER_SECTOR) new_sector(false);
  ESP_ERROR_CHECK(esp_partition_write(part, rec_offset(st.head_sector, st.head_used), r, sizeof(*r)));
  st.head_used++;
}

// Find the newest sector and how full it is. Only needed after a cold boot.
static void scan(void) {
  uint32_t *seqs = malloc(n_sectors * sizeof(uint32_t));
  assert(seqs);
  bool any = false;
  sector_hdr_t newest = {0};
  uint32_t newest_sector = 0;
  for (uint32_t i = 0; i < n_sectors; i++) {
    sector_hdr_t h;
    ESP_ERROR_CHECK(esp_partition_read(part, (size_t)i * SECTOR_SIZE, &h, sizeof(h)));
    bool ok = h.magic == HDR_MAGIC && h.seq != 0xFFFFFFFF && h.base <= h.seq;
    seqs[i] = ok ? h.seq : 0;
    if (ok && (!any || h.seq > newest.seq)) {
      any = true;
      newest = h;
      newest_sector = i;
    }
  }

  if (!any) {
    ESP_LOGW(TAG, "Flash log empty or unformatted: starting a new one");
    st.head_sector = n_sectors - 1;  // so the first sector written is 0
    st.head_seq = 0;
    st.base_seq = 1;
    new_sector(true);
    free(seqs);
    return;
  }

  st.head_sector = newest_sector;
  st.head_seq = newest.seq;
  st.base_seq = newest.base;
  if (st.head_seq - st.base_seq >= n_sectors) st.base_seq = st.head_seq - n_sectors + 1;
  // Every sector from base to head must be present (an interrupted erase could
  // leave a hole); drop anything older than a hole.
  for (uint32_t seq = st.head_seq; seq > st.base_seq; seq--) {
    if (seqs[sector_of_seq(seq - 1)] != seq - 1) {
      st.base_seq = seq;
      break;
    }
  }
  free(seqs);

  // Count records in the newest sector.
  flash_rec_t *recs = malloc(RECS_PER_SECTOR * sizeof(flash_rec_t));
  assert(recs);
  ESP_ERROR_CHECK(esp_partition_read(part, rec_offset(st.head_sector, 0), recs,
                                     RECS_PER_SECTOR * sizeof(flash_rec_t)));
  st.head_used = 0;
  while (st.head_used < RECS_PER_SECTOR && recs[st.head_used].ts != EMPTY_TS) st.head_used++;
  free(recs);
}

// ---------------------------------------------------------------------------
void datalog_init(bool cold_boot) {
  if (!lock) lock = xSemaphoreCreateMutex();
  off_cache_reset();

  part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, LOG_PARTITION_SUBTYPE, "thlog");
  if (!part) {
    ESP_LOGE(TAG, "No 'thlog' partition: was the partition table flashed?");
    abort();
  }
  n_sectors = part->size / SECTOR_SIZE;

  if (!nvs_ok) nvs_ok = nvs_open("thlog", NVS_READWRITE, &nvs) == ESP_OK;

  if (cold_boot || st.magic != RTC_STATE_MAGIC) {
    uint32_t boot = 0;
    if (nvs_ok) {
      nvs_get_u32(nvs, "boot", &boot);
      boot++;
      nvs_set_u32(nvs, "boot", boot);
      nvs_commit(nvs);
    }
    st.magic = 0;
    scan();
    st.boot = boot;
    st.magic = RTC_STATE_MAGIC;
  }
  ESP_LOGI(TAG, "Flash log: %lu sectors, seq %lu..%lu, %lu in newest, boot %lu",
           (unsigned long)n_sectors, (unsigned long)st.base_seq, (unsigned long)st.head_seq,
           (unsigned long)st.head_used, (unsigned long)st.boot);
}

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
  ulp_log_total = 0;
  ulp_flushed_total = 0;

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
  for (int i = 0; i < 100 && ulp_log_total == 0; i++) vTaskDelay(pdMS_TO_TICKS(10));
  vTaskDelay(pdMS_TO_TICKS(10));
  datalog_realign_after_reading();
}

void datalog_flush(void) {
  if (ulp_log_total == ulp_flushed_total) return;

  static th_record_t raw[TH_LP_RING_CAPACITY];
  LOCK();
  uint32_t total, seq;
  // Seqlock: retry if the LP core wrote a record while we were copying.
  do {
    while ((seq = ulp_log_seq) & 1) {}
    total = ulp_log_total;
    for (int i = 0; i < TH_LP_RING_CAPACITY; i++) raw[i] = LP_RING[i];
  } while (seq != ulp_log_seq);

  uint32_t done = ulp_flushed_total;
  if (total - done > TH_LP_RING_CAPACITY) {
    ESP_LOGW(TAG, "LP buffer overflowed: %lu readings lost", (unsigned long)(total - done - TH_LP_RING_CAPACITY));
    done = total - TH_LP_RING_CAPACITY;
  }

  // Convert LP-timer ticks to wall-clock time using the current RTC calibration.
  uint64_t ticks_now = rtc_time_get();
  int64_t t_now = now_us();
  uint32_t cal = esp_clk_slowclk_cal_get();

  for (uint32_t i = done; i != total; i++) {
    const th_record_t *r = &raw[i % TH_LP_RING_CAPACITY];
    uint64_t ticks = ((uint64_t)r->ticks_hi << 32) | r->ticks_lo;
    uint64_t age_us = ticks_now > ticks ? rtc_time_slowclk_to_us(ticks_now - ticks, cal) : 0;
    uint32_t ts = (uint32_t)((t_now - (int64_t)age_us) / 1000000LL);
    if (ts == 0) ts = 1;  // 0 means "time unknown" to clients
    flash_rec_t f = {
        .ts = ts,
        .boot = (uint16_t)st.boot,
        .temp = r->temp,
        .hum = r->hum,
        .reserved = 0xFFFF,
    };
    append(&f);
  }
  ulp_flushed_total = total;
  UNLOCK();
  ESP_LOGI(TAG, "Moved %lu readings to flash", (unsigned long)(total - done));
}

void datalog_range(uint32_t *first, uint32_t *next) {
  LOCK();
  *first = st.base_seq * RECS_PER_SECTOR;
  *next = st.head_seq * RECS_PER_SECTOR + st.head_used;
  UNLOCK();
}

// When the newest sector fills, the next write erases the oldest one, so the
// count drops by RECS_PER_SECTOR and climbs back to this.
uint32_t datalog_capacity(void) { return n_sectors * RECS_PER_SECTOR; }

uint16_t datalog_read(uint32_t start, reading_t *out, uint16_t n, uint32_t *actual_start) {
  static flash_rec_t buf[64];
  LOCK();
  uint32_t first = st.base_seq * RECS_PER_SECTOR;
  uint32_t next = st.head_seq * RECS_PER_SECTOR + st.head_used;
  if (start < first) start = first;
  if (start > next) start = next;
  if (n > next - start) n = next - start;
  *actual_start = start;

  uint16_t done = 0;
  while (done < n) {
    uint32_t idx = start + done;
    uint32_t seq = idx / RECS_PER_SECTOR, slot = idx % RECS_PER_SECTOR;
    uint32_t run = RECS_PER_SECTOR - slot;
    if (run > (uint32_t)(n - done)) run = n - done;
    if (run > 64) run = 64;
    ESP_ERROR_CHECK(esp_partition_read(part, rec_offset(sector_of_seq(seq), slot), buf, run * sizeof(flash_rec_t)));
    for (uint32_t i = 0; i < run; i++) {
      out[done + i].ts = resolve_ts(&buf[i]);
      out[done + i].temp = buf[i].temp;
      out[done + i].hum = buf[i].hum;
    }
    done += run;
  }
  UNLOCK();
  return n;
}

bool datalog_latest(reading_t *out) {
  uint32_t first, next, s;
  datalog_range(&first, &next);
  return next > first && datalog_read(next - 1, out, 1, &s) == 1;
}

void datalog_clear(void) {
  datalog_flush();
  LOCK();
  new_sector(true);
  UNLOCK();
  ESP_LOGW(TAG, "Log cleared");
}

void datalog_log_status(void) {
  uint32_t first, next;
  datalog_range(&first, &next);
  ESP_LOGI(TAG, "%lu readings in flash, LP %lu taken / %lu moved, last sensor status %lu (0 = ok)",
           (unsigned long)(next - first), (unsigned long)ulp_log_total, (unsigned long)ulp_flushed_total,
           (unsigned long)ulp_last_error);
}

uint32_t datalog_next_due(const reading_t *latest) {
  uint32_t now = datalog_now();
  if (scheduled_epoch > now) return scheduled_epoch;
  return latest && latest->ts ? latest->ts + TH_LOG_INTERVAL_S : now + TH_LOG_INTERVAL_S;
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
  // First time the clock is set this power-on: remember the offset so readings
  // already stored with seconds-since-power-on can be converted.
  if (!datalog_time_valid() && epoch >= MIN_VALID_EPOCH && nvs_ok) {
    char key[16];
    snprintf(key, sizeof(key), "o%u", (uint16_t)st.boot);
    int32_t off = (int32_t)(epoch - datalog_now());
    LOCK();
    nvs_set_i32(nvs, key, off);
    nvs_commit(nvs);
    off_cache_reset();
    UNLOCK();
  }

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
