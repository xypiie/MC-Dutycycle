// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Peter Kästle
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <stdio.h>
#include <string.h>
#include "session.h"

#define MC_FREQUENCY 869618000u
#define MC_STOP_FLAG 1u
#define MC_DIRECTORY EXT_PATH("apps_data/mc_dutycycle")

typedef struct {
    McSession session;
    FuriMutex* mutex;
    FuriMessageQueue* input;
    FuriThread* worker;
    const SubGhzDevice* radio;
    uint32_t frequency;
    bool saved;
    uint8_t save_status;
} McApp;

// TI CC1101 registers, not a LoRa demodulator. RX BW = 101.5625 kHz at
// 26 MHz. SYNC_MODE=0 prevents RSSI freezing on an accidental sync word;
// asynchronous mode and RXOFF_MODE=3 keep the receiver running continuously.
// https://www.ti.com/lit/ds/symlink/cc1101.pdf, sections 13 and 17.3.
static uint8_t rssi_preset[] = {
    0x00, 0x2E, 0x02, 0x2E, 0x07, 0x00, 0x08, 0x30, 0x0B, 0x06, 0x10, 0xC7, 0x11, 0x83, 0x12, 0x00,
    0x17, 0x0C, 0x18, 0x18, 0x19, 0x10, 0x1B, 0x03, 0x1C, 0x40, 0x1D, 0x91, 0x21, 0x56, 0x23, 0xE9,
    0x24, 0x2A, 0x25, 0x00, 0x26, 0x1F, 0x00, 0x00, 0,    0,    0,    0,    0,    0,    0,    0,
};

static uint32_t now_ms(void) {
    return (uint32_t)((uint64_t)furi_get_tick() * 1000 / furi_kernel_get_tick_frequency());
}

static int32_t sample_worker(void* context) {
    McApp* app = context;
    while(!(furi_thread_flags_get() & MC_STOP_FLAG)) {
        int16_t rssi = (int16_t)(subghz_devices_get_rssi(app->radio) * 2);
        uint32_t now = now_ms();
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        mc_session_sample(&app->session, now, rssi);
        furi_mutex_release(app->mutex);
        furi_delay_ms(1);
    }
    return 0;
}

static void draw(Canvas* canvas, void* context) {
    McApp* app = context;
    char line[64];
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    McSession* s = &app->session;
    McState state = s->state;
    int16_t rssi = s->rssi_half_dbm;
    int threshold = s->threshold_dbm;
    uint32_t duration = s->duration_ms, elapsed = s->elapsed_ms;
    uint32_t percent = mc_session_percent(s);
    uint32_t gap = s->max_gap_ms, lost = s->dropped_transitions;
    uint8_t save_status = app->save_status;
    furi_mutex_release(app->mutex);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "MC-Dutycycle");
    canvas_set_font(canvas, FontSecondary);
    const char* label = state == McCalibration ? "Calibrate" :
                        state == McArmed       ? "Armed: wait RF" :
                        state == McRecording   ? "Measuring" :
                                                 "Done";
    snprintf(line, sizeof(line), "%s  %lus", label, (unsigned long)(duration / 1000));
    canvas_draw_str(canvas, 2, 21, line);
    snprintf(line, sizeof(line), "RSSI %d dBm  T %d", rssi / 2, threshold);
    canvas_draw_str(canvas, 2, 31, line);
    if(state == McRecording || state == McDone) {
        snprintf(
            line,
            sizeof(line),
            "RF %lu.%02lu%%  %lu.%lus",
            (unsigned long)(percent / 100),
            (unsigned long)(percent % 100),
            (unsigned long)(elapsed / 1000),
            (unsigned long)(elapsed % 1000 / 100));
        canvas_draw_str(canvas, 2, 41, line);
        snprintf(
            line,
            sizeof(line),
            "gap %lums %s",
            (unsigned long)gap,
            lost             ? "LOG FULL" :
            save_status == 2 ? "SD ERROR" :
            save_status == 1 ? "CSV saved" :
                               "");
    } else {
        snprintf(line, sizeof(line), "869.618 MHz   U/D: dBm");
    }
    canvas_draw_str(canvas, 2, 51, line);
    canvas_draw_str(
        canvas,
        2,
        62,
        state == McRecording ? "OK/Back stop" :
        state == McArmed     ? "OK/Back cancel" :
                               "OK arm  L/R time  Back exit");
}

static void input_event(InputEvent* event, void* context) {
    McApp* app = context;
    if(event->type == InputTypeShort || event->type == InputTypeRepeat ||
       (event->key == InputKeyBack && event->type == InputTypeLong)) {
        furi_message_queue_put(app->input, event, 0);
    }
}

static bool file_text(File* file, const char* text) {
    size_t length = strlen(text);
    return storage_file_write(file, text, length) == length;
}

static void save_session(McApp* app) {
    // Called only after capture has stopped: SD writes cannot block RSSI sampling.
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    bool ready = app->session.state == McDone && !app->saved;
    if(ready) app->saved = true;
    furi_mutex_release(app->mutex);
    if(!ready) return;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, MC_DIRECTORY);
    char path[128], text[192];
    snprintf(
        path,
        sizeof(path),
        MC_DIRECTORY "/mc_%lu_%lu.csv",
        (unsigned long)furi_hal_rtc_get_timestamp(),
        (unsigned long)furi_get_tick());
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, path, FSAM_WRITE, FSOM_CREATE_NEW);
    McSession* s = &app->session;
    if(ok) {
        snprintf(
            text,
            sizeof(text),
            "# MC-Dutycycle, firmware=1.5.1, frequency_hz=%lu, rx_bw_hz=101563, threshold_dbm=%d\n",
            (unsigned long)app->frequency,
            s->threshold_dbm);
        ok = file_text(file, text);
        snprintf(
            text,
            sizeof(text),
            "# elapsed_ms=%lu,on_ms=%lu,samples=%lu,max_gap_ms=%lu,late_intervals=%lu,dropped_transitions=%lu\n",
            (unsigned long)s->elapsed_ms,
            (unsigned long)s->on_ms,
            (unsigned long)s->samples,
            (unsigned long)s->max_gap_ms,
            (unsigned long)s->late_intervals,
            (unsigned long)s->dropped_transitions);
        ok = file_text(file, text) && ok;
        ok = file_text(file, "elapsed_ms,rf_active,window_end\n") && ok;
        for(uint16_t i = 0; ok && i < s->transition_count; ++i) {
            uint32_t edge = s->transitions[i];
            snprintf(
                text,
                sizeof(text),
                "%lu,%u,%u\n",
                (unsigned long)(edge & MC_TIME_MASK),
                (unsigned int)((edge & MC_RF_ON) != 0),
                (unsigned int)((edge & MC_WINDOW_END) != 0));
            ok = file_text(file, text);
        }
    }
    bool closed = storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->save_status = ok && closed ? 1 : 2;
    furi_mutex_release(app->mutex);
}

int32_t mc_dutycycle_app(void* context) {
    UNUSED(context);
    McApp* app = malloc(sizeof(*app));
    memset(app, 0, sizeof(*app));
    mc_session_init(&app->session);
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input = furi_message_queue_alloc(16, sizeof(InputEvent));
    subghz_devices_init();
    app->radio = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    if(!app->radio || !subghz_devices_begin(app->radio)) {
        furi_message_queue_free(app->input);
        furi_mutex_free(app->mutex);
        subghz_devices_deinit();
        free(app);
        return -1;
    }
    subghz_devices_reset(app->radio);
    subghz_devices_load_preset(app->radio, FuriHalSubGhzPresetCustom, rssi_preset);
    app->frequency = subghz_devices_set_frequency(app->radio, MC_FREQUENCY);
    subghz_devices_set_rx(app->radio);
    furi_delay_ms(5);
    app->worker = furi_thread_alloc_ex("MC-RSSI", 2048, sample_worker, app);
    furi_thread_start(app->worker);
    Gui* gui = furi_record_open(RECORD_GUI);
    ViewPort* viewport = view_port_alloc();
    view_port_draw_callback_set(viewport, draw, app);
    view_port_input_callback_set(viewport, input_event, app);
    gui_add_view_port(gui, viewport, GuiLayerFullscreen);

    bool exit = false;
    while(!exit) {
        save_session(app);
        InputEvent event;
        if(furi_message_queue_get(app->input, &event, 20) == FuriStatusOk) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            McState old = app->session.state;
            bool adjusting = event.key == InputKeyUp || event.key == InputKeyDown ||
                             event.key == InputKeyLeft || event.key == InputKeyRight;
            bool rearm = adjusting && (old == McRecording || old == McArmed);
            if(adjusting || event.key == InputKeyOk || event.key == InputKeyBack) {
                mc_session_stop(&app->session, now_ms());
            }
            furi_mutex_release(app->mutex);
            save_session(app);
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            McSession* s = &app->session;
            if(event.key == InputKeyBack) {
                exit = old == McCalibration || old == McDone || event.type == InputTypeLong;
            } else if(event.key == InputKeyOk && old != McRecording && old != McArmed) {
                mc_session_arm(s);
                app->saved = false;
                app->save_status = 0;
            } else if(adjusting) {
                int threshold = s->threshold_dbm;
                int duration = (int)(s->duration_ms / 1000);
                if(event.key == InputKeyUp) ++threshold;
                if(event.key == InputKeyDown) --threshold;
                if(event.key == InputKeyRight) ++duration;
                if(event.key == InputKeyLeft) --duration;
                s->threshold_dbm = threshold < -120 ? -120 : threshold > -5 ? -5 : threshold;
                s->duration_ms = (duration < 1 ? 1 : duration > 60 ? 60 : duration) * 1000u;
                s->state = McCalibration;
                if(rearm) mc_session_arm(s);
                app->saved = false;
                app->save_status = 0;
            }
            furi_mutex_release(app->mutex);
        }
        view_port_update(viewport);
    }
    furi_thread_flags_set(furi_thread_get_id(app->worker), MC_STOP_FLAG);
    furi_thread_join(app->worker);
    save_session(app);
    gui_remove_view_port(gui, viewport);
    view_port_free(viewport);
    furi_record_close(RECORD_GUI);
    furi_thread_free(app->worker);
    subghz_devices_idle(app->radio);
    subghz_devices_sleep(app->radio);
    subghz_devices_end(app->radio);
    subghz_devices_deinit();
    furi_message_queue_free(app->input);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
