// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Peter Kästle
#include "session.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void start(McSession* s, uint32_t now, uint32_t duration) {
    mc_session_init(s);
    s->duration_ms = duration;
    mc_session_arm(s);
    mc_session_sample(s, now, -80);
    assert(s->state == McRecording);
}

static void test_full_session(void) {
    McSession s;
    McActivity view, frozen, detail;
    mc_session_init(&s);
    mc_session_overview(&s, &view);
    assert(view.count == 0);
    mc_session_arm(&s);
    mc_session_overview(&s, &view);
    assert(view.count == 0);

    // The overview grows from the left on a fixed 0..60 second scale.
    // Old activity remains visible after it leaves the ten-second graph.
    start(&s, 0, 60000);
    mc_session_sample(&s, 10, -200);
    mc_session_sample(&s, 6000, -200);
    mc_session_overview(&s, &view);
    assert(view.count == 11 && view.levels[0] == 1);
    for(uint8_t i = 1; i < view.count; ++i)
        assert(view.levels[i] == 0);
    mc_session_sample(&s, 31000, -80);
    mc_session_sample(&s, 31010, -200);
    mc_session_sample(&s, 60000, -200);
    mc_session_overview(&s, &view);
    assert(s.state == McDone && view.count == MC_ACTIVITY_POINTS && s.on_ms == 20);
    for(uint8_t i = 0; i < view.count; ++i) {
        assert(view.levels[i] == (i == 0 || i == 51));
    }
    mc_session_activity(&s, &detail);
    for(uint8_t i = 0; i < detail.count; ++i)
        assert(detail.levels[i] == 0);

    // An early stop keeps the configured scale and the unrecorded area
    // blank; later RSSI reads cannot alter the final view.
    start(&s, 100, 60000);
    mc_session_sample(&s, 200, -200);
    mc_session_stop(&s, 15100);
    mc_session_overview(&s, &frozen);
    assert(frozen.count == 26 && frozen.levels[0] == 1);
    mc_session_sample(&s, 50100, -80);
    mc_session_overview(&s, &view);
    assert(view.count == frozen.count);
    assert(memcmp(view.levels, frozen.levels, view.count) == 0);
    mc_session_arm(&s);
    mc_session_overview(&s, &view);
    assert(view.count == 0);
    mc_session_sample(&s, 60000, -80);
    mc_session_overview(&s, &view);
    assert(view.count == 1 && view.levels[0] == 1);

    // Short sessions use the same screen width with finer time intervals.
    start(&s, 0, 1000);
    mc_session_sample(&s, 500, -200);
    mc_session_sample(&s, 1000, -200);
    mc_session_overview(&s, &view);
    assert(view.count == MC_ACTIVITY_POINTS && mc_session_percent(&s) == 5000);
    for(uint8_t i = 0; i < view.count; ++i)
        assert(view.levels[i] == (i < 50));
    start(&s, 0, 60000);
    mc_session_sample(&s, 600, -200);
    mc_session_overview(&s, &view);
    assert(view.count == 2 && view.levels[0] == 1 && view.levels[1] == 0);

    // Both graph histories are independent of the finite CSV log.
    start(&s, 0, 60000);
    for(uint32_t t = 1; t <= 5000; ++t)
        mc_session_sample(&s, t, t % 2 ? -200 : -80);
    assert(s.dropped_transitions > 0);
    mc_session_sample(&s, 5001, -200);
    mc_session_sample(&s, 15000, -200);
    mc_session_sample(&s, 16000, -80);
    mc_session_sample(&s, 16100, -200);
    mc_session_overview(&s, &view);
    assert(view.count == 27 && view.levels[0] == 1 && view.levels[26] == 1);
    for(uint8_t i = 9; i < 26; ++i)
        assert(view.levels[i] == 0);
    assert(s.on_ms == 2601);

    start(&s, UINT32_MAX - 499, 60000);
    mc_session_sample(&s, 0, -200);
    mc_session_sample(&s, 10000, -200);
    mc_session_overview(&s, &view);
    assert(view.count == 18 && view.levels[0] == 1);
    for(uint8_t i = 1; i < view.count; ++i)
        assert(view.levels[i] == 0);
    start(&s, 0, 60000);
    mc_session_sample(&s, 600000, -80);
    mc_session_overview(&s, &view);
    assert(s.state == McDone && view.count == MC_ACTIVITY_POINTS && s.on_ms == 60000);
    for(uint8_t i = 0; i < view.count; ++i)
        assert(view.levels[i] == 1);
}

int main(void) {
    McSession s;
    McActivity view, frozen;
    mc_session_init(&s);
    mc_session_activity(&s, &view);
    assert(view.count == 0);
    mc_session_arm(&s);
    mc_session_activity(&s, &view);
    assert(view.count == 0);

    // A brief pulse stays visible in its 100 ms interval without changing
    // the actual on-time used for the percentage.
    start(&s, 1000, 60000);
    mc_session_activity(&s, &view);
    assert(view.count == 1 && view.levels[0] == 1);
    mc_session_sample(&s, 1010, -200);
    mc_session_sample(&s, 1100, -200);
    mc_session_activity(&s, &view);
    assert(view.count == 2 && view.levels[0] == 1 && view.levels[1] == 0);
    mc_session_sample(&s, 1150, -80);
    mc_session_sample(&s, 1160, -200);
    mc_session_activity(&s, &view);
    assert(view.count == 2 && view.levels[0] == 1 && view.levels[1] == 1);
    assert(s.on_ms == 20 && mc_session_percent(&s) == 1250);

    // Scroll exactly ten seconds between the oldest and newest points.
    start(&s, 0, 60000);
    mc_session_sample(&s, 10, -200);
    mc_session_sample(&s, 10000, -200);
    mc_session_activity(&s, &view);
    assert(view.count == MC_ACTIVITY_POINTS && view.levels[0] == 1);
    assert(view.levels[view.count - 1] == 0);
    mc_session_sample(&s, 10100, -200);
    mc_session_activity(&s, &view);
    for(uint8_t i = 0; i < view.count; ++i)
        assert(view.levels[i] == 0);
    mc_session_sample(&s, 11000, -80);
    mc_session_sample(&s, 11100, -200);
    mc_session_activity(&s, &view);
    assert(view.levels[view.count - 2] == 1 && view.levels[view.count - 1] == 0);

    // Stopping freezes the trace and does not invent a falling RF edge.
    start(&s, 0, 60000);
    mc_session_sample(&s, 250, -80);
    mc_session_stop(&s, 350);
    mc_session_activity(&s, &frozen);
    assert(s.state == McDone && frozen.count == 4);
    for(uint8_t i = 0; i < frozen.count; ++i)
        assert(frozen.levels[i] == 1);
    mc_session_sample(&s, 50000, -200);
    mc_session_activity(&s, &view);
    assert(view.count == frozen.count);
    assert(memcmp(view.levels, frozen.levels, view.count) == 0);
    mc_session_arm(&s);
    mc_session_activity(&s, &view);
    assert(view.count == 0);
    mc_session_sample(&s, 60000, -80);
    mc_session_activity(&s, &view);
    assert(view.count == 1 && view.levels[0] == 1);

    // Long gaps use the same held state as on-time integration and discard
    // stale points when the whole visible window has passed.
    start(&s, 0, 60000);
    mc_session_sample(&s, 25000, -200);
    mc_session_activity(&s, &view);
    assert(view.count == MC_ACTIVITY_POINTS && s.on_ms == 25000);
    for(uint8_t i = 0; i + 1 < view.count; ++i)
        assert(view.levels[i] == 1);
    assert(view.levels[view.count - 1] == 0);
    mc_session_sample(&s, 40000, -200);
    mc_session_activity(&s, &view);
    for(uint8_t i = 0; i < view.count; ++i)
        assert(view.levels[i] == 0);

    // An automatic stop on a ten-second boundary keeps both endpoints.
    start(&s, 0, 10000);
    mc_session_sample(&s, 10, -200);
    mc_session_sample(&s, 10000, -200);
    mc_session_activity(&s, &view);
    assert(s.state == McDone && view.count == MC_ACTIVITY_POINTS);
    assert(view.levels[0] == 1 && view.levels[view.count - 1] == 0);
    start(&s, 0, 10000);
    mc_session_sample(&s, 20000, -80);
    mc_session_activity(&s, &view);
    assert(s.state == McDone && s.elapsed_ms == 10000 && s.on_ms == 10000);
    for(uint8_t i = 0; i < view.count; ++i)
        assert(view.levels[i] == 1);

    // The rolling trace keeps updating after the CSV transition log fills.
    start(&s, 0, 60000);
    for(uint32_t t = 1; t <= 5000; ++t)
        mc_session_sample(&s, t, t % 2 ? -200 : -80);
    assert(s.transition_count == MC_TRANSITIONS && s.dropped_transitions > 0);
    mc_session_sample(&s, 5001, -200);
    mc_session_sample(&s, 15000, -200);
    mc_session_sample(&s, 16000, -80);
    mc_session_sample(&s, 16100, -200);
    mc_session_activity(&s, &view);
    for(uint8_t i = 0; i + 2 < view.count; ++i)
        assert(view.levels[i] == 0);
    assert(view.levels[view.count - 2] == 1 && view.levels[view.count - 1] == 0);

    start(&s, UINT32_MAX - 499, 60000);
    mc_session_sample(&s, 0, -200);
    mc_session_activity(&s, &view);
    assert(s.on_ms == 500 && view.count == 6 && view.levels[5] == 0);
    for(uint8_t i = 0; i < 5; ++i)
        assert(view.levels[i] == 1);
    mc_session_sample(&s, 10000, -200);
    mc_session_activity(&s, &view);
    assert(view.count == MC_ACTIVITY_POINTS);
    for(uint8_t i = 0; i < view.count; ++i)
        assert(view.levels[i] == 0);
    test_full_session();
    puts("MC-Dutycycle rolling and full-session activity tests passed");
    return 0;
}
