// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026, Peter Kästle
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <lib/subghz/devices/devices.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>
#include <stdio.h>
#include <string.h>
#include "session.h"
#include "receiver.h"

#define MC_STOP_FLAG 1u
#define MC_DIRECTORY EXT_PATH("apps_data/mc_dutycycle")

typedef struct {
    McSession session;
    McReceiver receiver;
    McSpectrum spectrum;
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
// Address zero terminates the Flipper preset list. IOCFG2 (address 0x00)
// is configured separately by set_frequency for the internal RF switch.
static uint8_t rssi_preset[] = {
    0x02, 0x2E, 0x07, 0x00, 0x08, 0x30, 0x0B, 0x06, 0x10, 0xC7, 0x11, 0x83, 0x12, 0x00, 0x17, 0x0C,
    0x18, 0x18, 0x19, 0x10, 0x1B, 0x03, 0x1C, 0x40, 0x1D, 0x91, 0x21, 0x56, 0x23, 0xE9, 0x24, 0x2A,
    0x25, 0x00, 0x26, 0x1F, 0x00, 0x00, 0,    0,    0,    0,    0,    0,    0,    0,
};

static uint32_t now_ms(void) {
    return (uint32_t)((uint64_t)furi_get_tick() * 1000 / furi_kernel_get_tick_frequency());
}

static int32_t sample_worker(void* context) {
    McApp* app = context;
    while(!(furi_thread_flags_get() & MC_STOP_FLAG)) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        McReadPlan plan = mc_receiver_plan(&app->receiver, &app->spectrum, app->session.state);
        furi_mutex_release(app->mutex);
        if(plan.retune) {
            subghz_devices_idle(app->radio);
            subghz_devices_set_frequency(app->radio, plan.frequency);
            subghz_devices_set_rx(app->radio);
            furi_delay_ms(MC_RX_SETTLE_MS);
        }
        int16_t rssi = (int16_t)(subghz_devices_get_rssi(app->radio) * 2);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        mc_receiver_sample(&app->receiver, &app->spectrum, &app->session, plan, now_ms(), rssi);
        furi_mutex_release(app->mutex);
        furi_delay_ms(1);
    }
    return 0;
}

static void draw_calibration(
    Canvas* canvas,
    const McSpectrum* spectrum,
    int16_t rssi,
    int threshold,
    uint32_t duration) {
    const int x = 24, y = 22, width = 100, height = 26;
    const int center = x + width / 2;
    char line[40];
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 0, 8, "869.618 MHz");
    snprintf(line, sizeof(line), "Cal %lus", (unsigned long)(duration / 1000));
    canvas_draw_str_aligned(canvas, 126, 8, AlignRight, AlignBottom, line);
    if(spectrum->valid_bins > MC_SPECTRUM_CENTER) {
        snprintf(line, sizeof(line), "RSSI %d  T %d dBm", rssi / 2, threshold);
    } else {
        snprintf(line, sizeof(line), "RSSI --  T %d dBm", threshold);
    }
    canvas_draw_str(canvas, 0, 18, line);
    canvas_draw_str(canvas, 14, y + 5, "0");
    canvas_draw_str(canvas, 0, y + height + 1, "-120");
    canvas_draw_line(canvas, x, y, x, y + height);
    canvas_draw_line(canvas, x, y + height, x + width, y + height);
    int threshold_y = mc_spectrum_y(threshold * 2, y, height);
    for(int i = x; i <= x + width; i += 4) {
        canvas_draw_line(canvas, i, threshold_y, i + 1, threshold_y);
    }
    for(int i = y; i < y + height; i += 4)
        canvas_draw_dot(canvas, center, i);
    // The bracket marks the nominal 62.5 kHz MeshCore channel.
    int half_channel = (31250u * width + MC_SPECTRUM_SPAN_HZ / 2) / MC_SPECTRUM_SPAN_HZ;
    canvas_draw_line(
        canvas, center - half_channel, y + height - 3, center - half_channel, y + height);
    canvas_draw_line(
        canvas, center + half_channel, y + height - 3, center + half_channel, y + height);
    for(uint8_t i = 0; i < spectrum->valid_bins; ++i) {
        int next_x = x + i * width / (MC_SPECTRUM_BINS - 1);
        int next_y = mc_spectrum_y(spectrum->levels[i], y, height);
        if(i) {
            int prev_x = x + (i - 1) * width / (MC_SPECTRUM_BINS - 1);
            int prev_y = mc_spectrum_y(spectrum->levels[i - 1], y, height);
            canvas_draw_line(canvas, prev_x, prev_y, next_x, next_y);
        } else {
            canvas_draw_dot(canvas, next_x, next_y);
        }
    }
    snprintf(line, sizeof(line), "-%luk", (unsigned long)(MC_SPECTRUM_SPAN_HZ / 2000));
    canvas_draw_str(canvas, x, 56, line);
    canvas_draw_str_aligned(canvas, center, 56, AlignCenter, AlignBottom, "0");
    snprintf(line, sizeof(line), "+%luk", (unsigned long)(MC_SPECTRUM_SPAN_HZ / 2000));
    canvas_draw_str_aligned(canvas, x + width, 56, AlignRight, AlignBottom, line);
    canvas_draw_str(canvas, 0, 64, "U/D T  L/R time  OK arm");
}

static void draw_activity_trace(
    Canvas* canvas,
    const McActivity* activity,
    int high,
    int low,
    bool right_aligned) {
    int x = 24;
    if(right_aligned) x += MC_ACTIVITY_POINTS - activity->count;
    canvas_draw_str(canvas, 8, high + 3, "1");
    canvas_draw_str(canvas, 8, low + 3, "0");
    canvas_draw_line(canvas, 20, high, 20, low);
    for(uint8_t i = 0; i < activity->count; ++i) {
        int next_x = x + i;
        int next_y = activity->levels[i] ? high : low;
        if(i) {
            int prev_y = activity->levels[i - 1] ? high : low;
            canvas_draw_line(canvas, next_x - 1, prev_y, next_x, prev_y);
            if(next_y != prev_y) canvas_draw_line(canvas, next_x, prev_y, next_x, next_y);
        } else {
            canvas_draw_dot(canvas, next_x, next_y);
        }
    }
}

static void draw_activity(
    Canvas* canvas,
    const McActivity* overview,
    const McActivity* detail,
    uint32_t duration,
    bool done) {
    char label[16];
    draw_activity_trace(canvas, overview, 11, 19, false);
    canvas_draw_str(canvas, 24, 26, "0s");
    snprintf(label, sizeof(label), "%lus", (unsigned long)(duration / 1000));
    canvas_draw_str_aligned(canvas, 124, 26, AlignRight, AlignBottom, label);
    draw_activity_trace(canvas, detail, 29, 37, true);
    canvas_draw_str(canvas, 24, 44, "-10s");
    canvas_draw_str_aligned(canvas, 124, 44, AlignRight, AlignBottom, done ? "end" : "now");
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
    McSpectrum spectrum = app->spectrum;
    McActivity activity, overview;
    mc_session_activity(s, &activity);
    mc_session_overview(s, &overview);
    furi_mutex_release(app->mutex);

    canvas_clear(canvas);
    if(state == McCalibration) {
        draw_calibration(canvas, &spectrum, rssi, threshold, duration);
        return;
    }
    bool capture = state == McRecording || state == McDone;
    if(!capture) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str(canvas, 2, 10, "MC-Dutycycle");
    }
    canvas_set_font(canvas, FontSecondary);
    if(!capture) {
        snprintf(line, sizeof(line), "Armed: wait RF  %lus", (unsigned long)(duration / 1000));
        canvas_draw_str(canvas, 2, 21, line);
        snprintf(line, sizeof(line), "RSSI %d dBm  T %d", rssi / 2, threshold);
        canvas_draw_str(canvas, 2, 31, line);
    }
    if(capture) {
        uint32_t remaining = elapsed < duration ? (duration - elapsed + 999) / 1000 : 0;
        snprintf(
            line,
            sizeof(line),
            "RF %lu.%02lu%% %lu.%lus -%lus",
            (unsigned long)(percent / 100),
            (unsigned long)(percent % 100),
            (unsigned long)(elapsed / 1000),
            (unsigned long)(elapsed % 1000 / 100),
            (unsigned long)remaining);
        canvas_draw_str(canvas, 2, 8, line);
        draw_activity(canvas, &overview, &activity, duration, state == McDone);
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
    canvas_draw_str(canvas, 2, capture ? 53 : 51, line);
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

static void show_radio_error(void) {
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, "MC-Dutycycle", 64, 4, AlignCenter, AlignTop);
    dialog_message_set_text(
        message, "Internal radio\nunavailable", 64, 30, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, NULL, "Back", NULL);
    dialog_message_show(dialogs, message);
    dialog_message_free(message);
    furi_record_close(RECORD_DIALOGS);
}

int32_t mc_dutycycle_app(void* context) {
    UNUSED(context);
    McApp* app = malloc(sizeof(*app));
    memset(app, 0, sizeof(*app));
    mc_session_init(&app->session);
    mc_receiver_init(&app->receiver, &app->spectrum);
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input = furi_message_queue_alloc(16, sizeof(InputEvent));
    subghz_devices_init();
    app->radio = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    // The internal device has no begin callback in firmware 1.5.1; the
    // generic begin wrapper returns false when that callback is absent.
    if(!app->radio || !subghz_devices_is_connect(app->radio)) {
        furi_message_queue_free(app->input);
        furi_mutex_free(app->mutex);
        subghz_devices_deinit();
        free(app);
        show_radio_error();
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
