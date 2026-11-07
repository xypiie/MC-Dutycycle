// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Peter Kästle
#include "session.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    McSession s;
    mc_session_init(&s);
    s.duration_ms = 1000;
    mc_session_arm(&s);
    mc_session_sample(&s, 10, -200);
    assert(s.state == McArmed && s.elapsed_ms == 0);
    mc_session_sample(&s, 50, -80);
    assert(s.state == McRecording && s.start_ms == 50);
    mc_session_sample(&s, 150, -200);
    mc_session_sample(&s, 250, -200);
    mc_session_sample(&s, 350, -80);
    mc_session_sample(&s, 1150, -80);
    assert(s.state == McDone && s.elapsed_ms == 1000 && s.on_ms == 800);
    assert(mc_session_percent(&s) == 8000);
    assert(s.transitions[0] == MC_RF_ON);
    assert(s.transitions[1] == 100);
    assert(s.transitions[2] == (300 | MC_RF_ON));
    assert(s.transitions[3] == (1000 | MC_WINDOW_END));
    assert(s.late_intervals > 0 && s.max_gap_ms >= 700);

    mc_session_arm(&s);
    assert(s.threshold_dbm == -60 && s.duration_ms == 1000);
    assert(s.on_ms == 0 && s.transition_count == 0);
    mc_session_sample(&s, UINT32_MAX - 499, -120);
    mc_session_sample(&s, 0, -200);
    mc_session_sample(&s, 500, -200);
    assert(s.state == McDone && s.on_ms == 500 && mc_session_percent(&s) == 5000);

    mc_session_arm(&s);
    mc_session_sample(&s, 0, -80);
    mc_session_stop(&s, 123);
    assert(s.state == McDone && s.on_ms == 123 && s.elapsed_ms == 123);
    mc_session_stop(&s, 456);
    assert(s.on_ms == 123);

    s.duration_ms = 60000;
    mc_session_arm(&s);
    mc_session_sample(&s, 0, -80);
    for(uint32_t t = 1; t <= 60000; ++t) {
        mc_session_sample(&s, t, t % 2 ? -200 : -80);
    }
    assert(s.on_ms == 30000 && mc_session_percent(&s) == 5000);
    assert(s.transition_count == MC_TRANSITIONS && s.dropped_transitions > 0);
    assert(s.max_gap_ms == 1 && s.late_intervals == 0);

    s.threshold_dbm = -45;
    mc_session_arm(&s);
    assert(s.threshold_dbm == -45 && s.state == McArmed && s.dropped_transitions == 0);
    mc_session_stop(&s, 0);
    assert(s.state == McCalibration);
    puts("MC-Dutycycle session tests passed");
    return 0;
}
