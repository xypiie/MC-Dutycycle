// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Peter Kästle
#include "receiver.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    McSession session;
    McReceiver receiver;
    McSpectrum spectrum;
    mc_session_init(&session);
    mc_receiver_init(&receiver, &spectrum);
    assert(spectrum.valid_bins == 0);
    for(uint8_t i = 0; i < MC_SPECTRUM_BINS; ++i) {
        McReadPlan plan = mc_receiver_plan(&receiver, &spectrum, session.state);
        assert(plan.calibration && plan.retune && plan.bin == i);
        assert(plan.frequency == MC_FREQUENCY - 600000u + i * 30000u);
        int16_t rssi = i == MC_SPECTRUM_CENTER ? -160 : -220;
        mc_receiver_sample(&receiver, &spectrum, &session, plan, i * 5, rssi);
        assert(session.state == McCalibration && session.on_ms == 0 && session.samples == 0);
    }
    assert(spectrum.valid_bins == MC_SPECTRUM_BINS);
    assert(mc_spectrum_frequency(0) == 869018000u);
    assert(mc_spectrum_frequency(MC_SPECTRUM_BINS - 1) == 870218000u);
    assert(spectrum.levels[MC_SPECTRUM_CENTER] == -160);
    assert(session.rssi_half_dbm == -160);
    McReadPlan plan = mc_receiver_plan(&receiver, &spectrum, session.state);
    assert(plan.bin == 0 && plan.frequency == MC_FREQUENCY - 600000u);

    // Arming while an off-center sweep read is in flight cannot trigger RF.
    session.duration_ms = 1000;
    mc_session_arm(&session);
    mc_receiver_sample(&receiver, &spectrum, &session, plan, 300, -60);
    assert(session.state == McArmed && session.samples == 0 && session.on_ms == 0);
    plan = mc_receiver_plan(&receiver, &spectrum, session.state);
    assert(!plan.calibration && plan.retune && plan.frequency == MC_FREQUENCY);
    mc_receiver_sample(&receiver, &spectrum, &session, plan, 305, -220);
    assert(session.state == McArmed);

    // Armed and recording reads stay at the center with no more retuning.
    plan = mc_receiver_plan(&receiver, &spectrum, session.state);
    assert(!plan.calibration && !plan.retune && plan.frequency == MC_FREQUENCY);
    mc_receiver_sample(&receiver, &spectrum, &session, plan, 400, -80);
    assert(session.state == McRecording && session.start_ms == 400);
    plan = mc_receiver_plan(&receiver, &spectrum, session.state);
    assert(!plan.retune && plan.frequency == MC_FREQUENCY);
    mc_receiver_sample(&receiver, &spectrum, &session, plan, 900, -220);
    mc_receiver_sample(&receiver, &spectrum, &session, plan, 1400, -220);
    assert(session.state == McDone && mc_session_percent(&session) == 5000);
    plan = mc_receiver_plan(&receiver, &spectrum, session.state);
    assert(!plan.retune && plan.frequency == MC_FREQUENCY);

    // Returning to calibration drops a pending fixed-frequency read and
    // clears the old trace before starting at the lower edge again.
    session.state = McCalibration;
    int16_t previous = session.rssi_half_dbm;
    mc_receiver_sample(&receiver, &spectrum, &session, plan, 1500, -60);
    assert(session.rssi_half_dbm == previous);
    plan = mc_receiver_plan(&receiver, &spectrum, session.state);
    assert(plan.calibration && plan.retune && plan.bin == 0);
    assert(spectrum.valid_bins == 0);

    // Even when arming at the center sweep bin, force a clean RX settle.
    for(uint8_t i = 0; i <= MC_SPECTRUM_CENTER; ++i) {
        plan = mc_receiver_plan(&receiver, &spectrum, session.state);
        mc_receiver_sample(&receiver, &spectrum, &session, plan, 1600 + i * 5, -220);
    }
    assert(receiver.frequency == MC_FREQUENCY);
    mc_session_arm(&session);
    plan = mc_receiver_plan(&receiver, &spectrum, session.state);
    assert(!plan.calibration && plan.retune && plan.frequency == MC_FREQUENCY);

    // Plot coordinates clamp weak/strong signals and move monotonically.
    assert(mc_spectrum_y(-240, 22, 26) == 48);
    assert(mc_spectrum_y(0, 22, 26) == 22);
    assert(mc_spectrum_y(-300, 22, 26) == 48);
    assert(mc_spectrum_y(20, 22, 26) == 22);
    uint8_t last_y = 48;
    for(int16_t rssi = -300; rssi <= 20; ++rssi) {
        uint8_t y = mc_spectrum_y(rssi, 22, 26);
        assert(y >= 22 && y <= 48 && y <= last_y);
        last_y = y;
    }
    puts("MC-Dutycycle receiver tests passed");
    return 0;
}
