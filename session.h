// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Peter Kästle
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define MC_TRANSITIONS 2048
#define MC_RF_ON       UINT32_C(0x80000000)
#define MC_WINDOW_END  UINT32_C(0x40000000)
#define MC_TIME_MASK   UINT32_C(0x3fffffff)

typedef enum {
    McCalibration,
    McArmed,
    McRecording,
    McDone
} McState;

typedef struct {
    McState state;
    int16_t threshold_dbm, rssi_half_dbm;
    uint32_t duration_ms, start_ms, last_ms, elapsed_ms, on_ms;
    uint32_t samples, max_gap_ms, late_intervals, dropped_transitions;
    bool rf_on;
    uint16_t transition_count;
    uint32_t transitions[MC_TRANSITIONS];
} McSession;

void mc_session_init(McSession* session);
void mc_session_arm(McSession* session);
void mc_session_sample(McSession* session, uint32_t now_ms, int16_t rssi_half_dbm);
void mc_session_stop(McSession* session, uint32_t now_ms);
uint32_t mc_session_percent(const McSession* session);
