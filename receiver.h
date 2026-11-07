// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Peter Kästle
#pragma once
#include "session.h"

#define MC_FREQUENCY        869618000u
#define MC_SPECTRUM_BINS    41u
#define MC_SPECTRUM_STEP_HZ 30000u
#define MC_SPECTRUM_CENTER  (MC_SPECTRUM_BINS / 2)
#define MC_SPECTRUM_SPAN_HZ ((MC_SPECTRUM_BINS - 1) * MC_SPECTRUM_STEP_HZ)
#define MC_RX_SETTLE_MS     3u

typedef struct {
    int16_t levels[MC_SPECTRUM_BINS];
    uint8_t valid_bins;
} McSpectrum;

typedef struct {
    uint32_t frequency;
    uint8_t next_bin;
    bool scanning;
} McReceiver;

typedef struct {
    uint32_t frequency;
    uint8_t bin;
    bool calibration, retune;
} McReadPlan;

void mc_receiver_init(McReceiver* receiver, McSpectrum* spectrum);
McReadPlan mc_receiver_plan(McReceiver* receiver, McSpectrum* spectrum, McState state);
void mc_receiver_sample(
    McReceiver* receiver,
    McSpectrum* spectrum,
    McSession* session,
    McReadPlan plan,
    uint32_t now_ms,
    int16_t rssi_half_dbm);
uint32_t mc_spectrum_frequency(uint8_t bin);
uint8_t mc_spectrum_y(int16_t rssi_half_dbm, uint8_t top, uint8_t height);
