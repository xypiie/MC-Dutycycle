// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Peter Kästle
#include "receiver.h"
#include <string.h>

static void spectrum_clear(McSpectrum* spectrum) {
    for(uint8_t i = 0; i < MC_SPECTRUM_BINS; ++i)
        spectrum->levels[i] = -240;
    spectrum->valid_bins = 0;
}

void mc_receiver_init(McReceiver* receiver, McSpectrum* spectrum) {
    memset(receiver, 0, sizeof(*receiver));
    spectrum_clear(spectrum);
}

uint32_t mc_spectrum_frequency(uint8_t bin) {
    return MC_FREQUENCY - MC_SPECTRUM_SPAN_HZ / 2 + bin * MC_SPECTRUM_STEP_HZ;
}

McReadPlan mc_receiver_plan(McReceiver* receiver, McSpectrum* spectrum, McState state) {
    McReadPlan plan = {0};
    plan.calibration = state == McCalibration;
    if(plan.calibration) {
        if(!receiver->scanning) {
            receiver->next_bin = 0;
            spectrum_clear(spectrum);
        }
        plan.bin = receiver->next_bin;
        plan.frequency = mc_spectrum_frequency(plan.bin);
        plan.retune = true;
    } else {
        plan.frequency = MC_FREQUENCY;
        // Always settle at the center when leaving calibration, including
        // when the last sweep bin happened to be the center bin itself.
        plan.retune = receiver->scanning || receiver->frequency != MC_FREQUENCY;
    }
    receiver->scanning = plan.calibration;
    receiver->frequency = plan.frequency;
    return plan;
}

void mc_receiver_sample(
    McReceiver* receiver,
    McSpectrum* spectrum,
    McSession* session,
    McReadPlan plan,
    uint32_t now,
    int16_t rssi) {
    // Input can change mode while the worker is retuning. A sweep sample
    // must never trigger an armed capture or enter its airtime calculation.
    if(plan.calibration != (session->state == McCalibration)) return;
    if(plan.calibration) {
        spectrum->levels[plan.bin] = rssi;
        if(spectrum->valid_bins <= plan.bin) spectrum->valid_bins = plan.bin + 1;
        if(plan.bin == MC_SPECTRUM_CENTER) session->rssi_half_dbm = rssi;
        receiver->next_bin = (plan.bin + 1) % MC_SPECTRUM_BINS;
    } else {
        mc_session_sample(session, now, rssi);
    }
}

uint8_t mc_spectrum_y(int16_t rssi, uint8_t top, uint8_t height) {
    // A fixed -120..0 dBm scale keeps the threshold stable between sweeps.
    int32_t level = rssi;
    if(level < -240) level = -240;
    if(level > 0) level = 0;
    return top + height - (level + 240) * height / 240;
}
