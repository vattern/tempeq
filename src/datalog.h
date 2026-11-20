#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "config.h"

// A reading with its timestamp resolved to unix time (UTC). Before the clock
// has been set over BLE, timestamps are seconds since power-on.
typedef struct {
  uint32_t ts;
  int16_t  temp;   // centi-degC, TH_TEMP_ERROR on sensor failure
  uint16_t hum;    // centi-%RH, TH_HUM_ERROR on sensor failure
} reading_t;

// Load the LP-core program and start the 15-minute reading schedule.
// Only call on a cold boot; it wipes the log.
void datalog_start_lp_core(void);

// Copy the log (oldest first) into `out`; returns the number of readings.
uint16_t datalog_snapshot(reading_t out[TH_LOG_CAPACITY]);

// Debug: log reading count and the LP core's last sensor error code.
void datalog_log_status(void);

// Estimated time of the next reading, given the latest one (NULL if none).
uint32_t datalog_next_due(const reading_t *latest);

uint32_t datalog_now(void);
bool datalog_time_valid(void);

// Set the wall clock. Stored readings are re-timestamped automatically and the
// LP timer is realigned so readings fall on :00/:15/:30/:45.
void datalog_set_time(uint32_t epoch);

// Correct LP-timer drift relative to the wall clock. Call right after an LP-core
// triggered wake (i.e. just after a reading).
void datalog_realign_after_reading(void);
