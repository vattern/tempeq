#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "config.h"

// A reading with its timestamp resolved to unix time (UTC).
// Readings taken before the clock was first set during the current power-on
// have ts = seconds since power-on (< MIN_VALID_EPOCH); they are converted to
// unix time once the clock is set. Readings from an earlier power-on whose
// clock was never set have ts = 0 (time unknown).
typedef struct {
  uint32_t ts;
  int16_t  temp;   // centi-degC, TH_TEMP_ERROR on sensor failure
  uint16_t hum;    // centi-%RH, TH_HUM_ERROR on sensor failure
} reading_t;

// Open the flash log. Call on every wake, after nvs_flash_init(), before any
// other datalog call. `cold_boot` starts a new power-on session.
void datalog_init(bool cold_boot);

// Load the LP-core program and start the reading schedule.
// Only call on a cold boot.
void datalog_start_lp_core(void);

// Move readings from the LP core's staging buffer to the flash log. Cheap when
// there is nothing new; safe to call often.
void datalog_flush(void);

// The flash log holds readings with record numbers first..next-1 (oldest first).
// Record numbers keep counting up across wrap-around and clears, so a client can
// remember `next` and later fetch only newer readings.
void datalog_range(uint32_t *first, uint32_t *next);

// Most readings the flash log can hold. Once full, each new sector overwrites
// the oldest RECS_PER_SECTOR (340) readings.
uint32_t datalog_capacity(void);

// Read up to `n` readings starting at record number `start` (clamped to the
// range). Returns how many were read and sets *actual_start.
uint16_t datalog_read(uint32_t start, reading_t *out, uint16_t n, uint32_t *actual_start);

// Latest reading; false if the log is empty.
bool datalog_latest(reading_t *out);

// Delete every reading (flash log and LP staging buffer).
void datalog_clear(void);

// Debug: log counts and the LP core's last sensor error code.
void datalog_log_status(void);

// Estimated time of the next reading, given the latest one (NULL if none).
uint32_t datalog_next_due(const reading_t *latest);

uint32_t datalog_now(void);
bool datalog_time_valid(void);

// Set the wall clock. Readings taken earlier in this power-on are re-timestamped
// automatically and the LP timer is realigned to whole TH_LOG_INTERVAL_S slots.
void datalog_set_time(uint32_t epoch);

// Correct LP-timer drift relative to the wall clock. Call right after an LP-core
// triggered wake (i.e. just after a reading).
void datalog_realign_after_reading(void);
