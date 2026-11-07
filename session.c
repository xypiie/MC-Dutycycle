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

static void activity_move(McSession* s, uint32_t bin) {
    if(bin <= s->activity_bin) return;
    if(bin - s->activity_bin >= MC_ACTIVITY_POINTS) {
        memset(s->activity, 0, sizeof(s->activity));
    } else {
        for(uint32_t i = s->activity_bin + 1; i <= bin; ++i) {
            s->activity[i % MC_ACTIVITY_POINTS] = 0;
        }
    }
    s->activity_bin = bin;
}

static uint32_t overview_bin(const McSession* s, uint32_t elapsed) {
    return s->duration_ms ? elapsed * (MC_ACTIVITY_POINTS - 1) / s->duration_ms : 0;
}

static void activity_observe(McSession* s, uint32_t elapsed, bool on) {
    uint32_t bin = elapsed / MC_ACTIVITY_STEP_MS;
    activity_move(s, bin);
    if(on) {
        s->activity[bin % MC_ACTIVITY_POINTS] = 1;
        s->overview[overview_bin(s, elapsed)] = 1;
    }
}

static void activity_advance(McSession* s, uint32_t elapsed) {
    if(elapsed == s->elapsed_ms) return;
    uint32_t first = s->elapsed_ms / MC_ACTIVITY_STEP_MS;
    uint32_t last = (elapsed - 1) / MC_ACTIVITY_STEP_MS;
    activity_move(s, last);
    // Match the time integration: hold the preceding RF state across gaps.
    // Updating at most one window also bounds work after a long pause.
    if(s->rf_on) {
        for(uint32_t i = overview_bin(s, s->elapsed_ms); i <= overview_bin(s, elapsed - 1); ++i) {
            s->overview[i] = 1;
        }
        if(last - first >= MC_ACTIVITY_POINTS) first = last - MC_ACTIVITY_POINTS + 1;
        for(uint32_t i = first; i <= last; ++i) {
            s->activity[i % MC_ACTIVITY_POINTS] = 1;
        }
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
    activity_advance(s, elapsed);
    if(s->rf_on) s->on_ms += step;
    s->elapsed_ms = elapsed;
    s->last_ms = now;
}

void mc_session_stop(McSession* s, uint32_t now) {
    if(s->state == McArmed) {
        s->state = McCalibration;
    } else if(s->state == McRecording) {
        advance(s, now);
        // Keep the final observed/held level; ending a session is not an
        // observed RF falling edge in the activity graph.
        activity_observe(s, s->elapsed_ms, s->rf_on);
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
        activity_observe(s, 0, true);
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
        if(s->state == McRecording) activity_observe(s, s->elapsed_ms, on);
    }
}

uint32_t mc_session_percent(const McSession* s) {
    return s->elapsed_ms ? (s->on_ms * 10000u + s->elapsed_ms / 2) / s->elapsed_ms : 0;
}

void mc_session_activity(const McSession* s, McActivity* activity) {
    activity->count = 0;
    if(s->state != McRecording && s->state != McDone) return;
    uint32_t count = s->activity_bin + 1;
    if(count > MC_ACTIVITY_POINTS) count = MC_ACTIVITY_POINTS;
    uint32_t first = s->activity_bin + 1 - count;
    for(uint32_t i = 0; i < count; ++i) {
        activity->levels[i] = s->activity[(first + i) % MC_ACTIVITY_POINTS];
    }
    activity->count = count;
}

void mc_session_overview(const McSession* s, McActivity* activity) {
    activity->count = 0;
    if(s->state != McRecording && s->state != McDone) return;
    activity->count = overview_bin(s, s->elapsed_ms) + 1;
    memcpy(activity->levels, s->overview, activity->count);
}
