// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Peter Kästle
#include "session.h"
#include <string.h>

static void transition(McSession* s, uint32_t value) {
    if(s->transition_count < MC_TRANSITIONS) {
        s->transitions[s->transition_count++] = value;
    } else {
        ++s->dropped_transitions;
    }
}

void mc_session_init(McSession* s) {
    memset(s, 0, sizeof(*s));
    s->threshold_dbm = -60;
    s->rssi_half_dbm = -240;
    s->duration_ms = 60000;
}

void mc_session_arm(McSession* s) {
    int16_t threshold = s->threshold_dbm, rssi = s->rssi_half_dbm;
    uint32_t duration = s->duration_ms;
    memset(s, 0, sizeof(*s));
    s->threshold_dbm = threshold;
    s->rssi_half_dbm = rssi;
    s->duration_ms = duration;
    s->state = McArmed;
}

static void advance(McSession* s, uint32_t now) {
    uint32_t elapsed = now - s->start_ms;
    if(elapsed > s->duration_ms) elapsed = s->duration_ms;
    uint32_t step = elapsed - s->elapsed_ms;
    uint32_t gap = now - s->last_ms;
    if(gap > s->max_gap_ms) s->max_gap_ms = gap;
    if(gap > 5) ++s->late_intervals;
    if(s->rf_on) s->on_ms += step;
    s->elapsed_ms = elapsed;
    s->last_ms = now;
}

void mc_session_stop(McSession* s, uint32_t now) {
    if(s->state == McArmed) {
        s->state = McCalibration;
    } else if(s->state == McRecording) {
        advance(s, now);
        transition(s, s->elapsed_ms | MC_WINDOW_END);
        s->rf_on = false;
        s->state = McDone;
    }
}

void mc_session_sample(McSession* s, uint32_t now, int16_t rssi) {
    s->rssi_half_dbm = rssi;
    bool on = rssi >= s->threshold_dbm * 2;
    if(s->state == McArmed && on) {
        s->start_ms = s->last_ms = now;
        s->rf_on = true;
        s->state = McRecording;
        s->samples = 1;
        transition(s, MC_RF_ON);
    } else if(s->state == McRecording) {
        ++s->samples;
        advance(s, now);
        if(s->elapsed_ms == s->duration_ms) {
            mc_session_stop(s, now);
        } else if(on != s->rf_on) {
            s->rf_on = on;
            transition(s, s->elapsed_ms | (on ? MC_RF_ON : 0));
        }
    }
}

uint32_t mc_session_percent(const McSession* s) {
    return s->elapsed_ms ? (s->on_ms * 10000u + s->elapsed_ms / 2) / s->elapsed_ms : 0;
}
