# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, Peter Kästle
"""Exercise the app entry point with the firmware 1.5.1 radio contract.

The host shim supplies the internal device's absent begin callback, a fake GUI,
and a Back event. The real entry point must reach the GUI and clean up on exit.
The worker is also exercised with a simulated radio, including an arm event
during retuning. Actual RSSI behavior and rendering still require hardware.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'mc_dutycycle.c').read_text()
app_type = re.search(r'typedef struct \{.*?\} McApp;', source, re.S).group()
entry_point = source[source.index('int32_t mc_dutycycle_app('):]
worker = source[source.index('static int32_t sample_worker('):
                source.index('static void draw_calibration(')]
shim = r'''
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "session.h"
#include "receiver.h"
typedef void FuriMutex;
typedef void FuriMessageQueue;
typedef void FuriThread;
typedef void Gui;
typedef void ViewPort;
typedef struct { bool (*begin)(void); } SubGhzDevice;
typedef struct { int key, type; } InputEvent;
enum { InputKeyBack, InputKeyUp, InputKeyDown, InputKeyLeft, InputKeyRight,
       InputKeyOk, InputTypeLong, FuriStatusOk, FuriWaitForever,
       FuriMutexTypeNormal, GuiLayerFullscreen, FuriHalSubGhzPresetCustom };
static const SubGhzDevice internal_radio = {.begin = NULL};
static int shown, began, reset, rx, ended, registry, errors;
static bool available = true;
static uint8_t rssi_preset[2];
static uint32_t radio_frequency, clock_ms;
static int radio_phase, tunes, idle, worker_steps, worker_limit;
static McSession* arm_during_retune;
#define UNUSED(value) (void)(value)
#define MC_STOP_FLAG 1u
#define SUBGHZ_DEVICE_CC1101_INT_NAME "cc1101_int"
#define RECORD_GUI "gui"
#define FURI_LOG_E(...) ((void)0)
#define furi_mutex_alloc(...) malloc(1)
#define furi_mutex_acquire(...) ((void)0)
#define furi_mutex_release(...) ((void)0)
#define furi_mutex_free free
#define furi_message_queue_alloc(...) malloc(1)
#define furi_message_queue_free free
#define furi_thread_alloc_ex(...) malloc(1)
#define furi_thread_start(...) ((void)0)
#define furi_thread_flags_set(...) ((void)0)
#define furi_thread_join(...) ((void)0)
#define furi_thread_free free
#define furi_record_open(...) ((Gui*)1)
#define furi_record_close(...) ((void)0)
#define view_port_alloc() malloc(1)
#define view_port_draw_callback_set(...) ((void)0)
#define view_port_input_callback_set(...) ((void)0)
#define view_port_update(...) ((void)0)
#define view_port_free free
#define gui_add_view_port(gui, port, layer) ((void)(gui), (void)(port), ++shown)
#define gui_remove_view_port(...) ((void)0)
#define subghz_devices_init() (++registry)
#define subghz_devices_deinit() (--registry)
#define subghz_devices_get_by_name(...) (available ? &internal_radio : NULL)
#define subghz_devices_is_connect(...) true
#define subghz_devices_reset(...) (++reset)
#define subghz_devices_load_preset(...) ((void)rssi_preset)
#define subghz_devices_sleep(...) ((void)0)
#define subghz_devices_end(...) (++ended)
#define now_ms() clock_ms
#define save_session(...) ((void)0)
#define show_radio_error() (++errors)
bool subghz_devices_begin(const SubGhzDevice* device) {
    ++began;
    return device->begin ? device->begin() : false;
}
void furi_delay_ms(uint32_t ms) { clock_ms += ms; }
uint32_t furi_thread_flags_get(void) {
    return worker_steps++ >= worker_limit ? MC_STOP_FLAG : 0;
}
void subghz_devices_idle(const SubGhzDevice* device) {
    (void)device;
    radio_phase = 0;
    ++idle;
}
uint32_t subghz_devices_set_frequency(const SubGhzDevice* device, uint32_t frequency) {
    (void)device;
    assert(radio_phase == 0);
    radio_frequency = frequency;
    ++tunes;
    if(arm_during_retune && frequency != MC_FREQUENCY) {
        mc_session_arm(arm_during_retune);
        arm_during_retune = NULL;
    }
    return frequency;
}
void subghz_devices_set_rx(const SubGhzDevice* device) {
    (void)device;
    radio_phase = 1;
    ++rx;
}
float subghz_devices_get_rssi(const SubGhzDevice* device) {
    (void)device;
    assert(radio_phase == 1);
    return radio_frequency == MC_FREQUENCY ? -40.0f : -10.0f;
}
int furi_message_queue_get(FuriMessageQueue* queue, InputEvent* event, int timeout) {
    (void)queue;
    (void)timeout;
    event->key = InputKeyBack;
    event->type = 0;
    return FuriStatusOk;
}
'''
checks = r'''
int main(void) {
    int result = mc_dutycycle_app(NULL);
    if(result != 0 || shown != 1 || reset != 1 || rx != 1 || ended != 1 || registry) {
        fprintf(stderr, "Startup failed: result=%d UI=%d begin=%d reset=%d RX=%d end=%d registry=%d\n",
                result, shown, began, reset, rx, ended, registry);
        return 1;
    }
    available = false;
    result = mc_dutycycle_app(NULL);
    if(result != -1 || registry || shown != 1 || errors != 1) {
        fprintf(stderr, "Unavailable-radio error path failed\n");
        return 1;
    }
    puts("Startup smoke test passed: GUI setup and clean shutdown reached");
    puts("Unavailable radio requests an error dialog and releases the registry");

    McApp app = {0};
    mc_session_init(&app.session);
    mc_receiver_init(&app.receiver, &app.spectrum);
    app.radio = &internal_radio;
    app.session.duration_ms = 1000;
    clock_ms = 0;
    tunes = idle = rx = worker_steps = 0;
    worker_limit = 3;
    arm_during_retune = &app.session;
    sample_worker(&app);
    assert(tunes == 2 && idle == 2 && rx == 2);
    assert(radio_frequency == MC_FREQUENCY);
    assert(app.session.state == McRecording);
    assert(app.session.start_ms == 2 * MC_RX_SETTLE_MS + 1);
    assert(app.session.on_ms == 1 && app.session.samples == 2);
    puts("Worker handoff passed: off-center RSSI ignored, center settled before capture");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='test-startup-', dir=root / '.venv') as directory:
    test_c = Path(directory) / 'test_startup.c'
    executable = Path(directory) / 'test_startup'
    test_c.write_text(shim + app_type + '\n' + worker + entry_point + checks)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-I', str(root),
                    str(test_c), str(root / 'session.c'), str(root / 'receiver.c'),
                    '-o', str(executable)],
                   check=True)
    raise SystemExit(subprocess.run([str(executable)]).returncode)
