# MC-Dutycycle

Receive-only RF occupancy meter for Flipper Zero, written by Peter Kästle.
BSD-2-Clause licensed. Built for the official **1.5.1** firmware.

MC-Dutycycle measures time above an adjustable RSSI threshold at
**869.618 MHz**, the MeshCore EU/Narrow frequency. It uses the internal CC1101
with a continuous 101.5625 kHz receive bandwidth, covering the 62.5 kHz LoRa
channel. It measures channel energy; it does not decode LoRa or identify a
particular repeater. Nearby transmitters and noise can contribute to the result.

## Build

On Linux, install Python 3 with venv support. For the host tests, also install
a C compiler (`cc`). From this directory:

```sh
python3 build.py setup
python3 build.py
python3 build.py test
```

The first setup/build needs internet access. The wrapper creates a persistent
`.venv` **in this directory**, installs uFBT from `requirements-build.txt`,
downloads the firmware **1.5.1** SDK, and installs its ARM toolchain beneath
`.venv/ufbt/`. Python dependencies and the download cache also live in `.venv`.
The environment remains available after reboot; no activation is required.
Subsequent builds reuse it. `.venv/`, generated editor files, and build output
are ignored by Git.

The result is **`dist/mc_dutycycle.fap`**. Copy it to the Flipper SD card's
`apps/Sub-GHz/` directory, for example with qFlipper. Open it under
Apps → Sub-GHz → MC-Dutycycle.

For direct uFBT commands, first `source .venv/bin/activate` from this directory.
The checked-in `.env` sets `UFBT_HOME=.venv/ufbt`, keeping the SDK and compiler
local. The build wrapper pins the SDK to 1.5.1 on each build.

## Measure an attended short session

1. Open the app and observe the live spectrum with the repeater quiet. The
   trace sweeps **869.018–870.218 MHz** (±600 kHz) in 30 kHz steps around the
   MeshCore frequency. The center marker is **869.618 MHz**; the two lower ticks mark
   the nominal 62.5 kHz channel. The vertical scale is fixed at -120 to 0 dBm,
   and the dashed horizontal line shows the threshold. **RSSI** above the graph
   is the latest center-frequency reading. Use **Up/Down** to
   change the threshold by 1 dB, from -120 to -5 dBm. Up raises the threshold
   and makes detection less sensitive. Pick a threshold above the ambient level
   and below the received repeater signal. Avoid placing it so close that the
   receiver saturates.
2. Use **Left/Right** to select a **1–60 second** measurement window. The default
   is 60 seconds.
3. Press **OK** to arm. Scanning stops, the receiver returns to 869.618 MHz,
   and it settles before sampling. The first sample at or above the threshold
   starts the timed measurement. Start the repeater's test after arming the Flipper.
4. Read the status line, for example **`RF 50.00% 50.0s -10s`**: RF percentage,
   elapsed time, and remaining seconds rounded up. Below it, the upper activity
   graph covers the **full configured duration** and the lower graph scrolls
   over the **last 10 seconds**. **1** means RF above the threshold was detected;
   **0** means no RF was detected in that interval. The percentage is detected
   on-time divided by elapsed measurement time. The window ends automatically
   after the selected duration. **OK** or **Back** stops it early.
5. A completed capture is saved to CSV. Press OK to arm again, or Back to exit.
   Back while armed cancels arming. Hold Back to exit from any state.

Changing the threshold or duration during a capture saves that segment and
rearms a fresh measurement. Captures never mix thresholds. During calibration,
changes simply update the settings. Settings reset to defaults on app launch.

This is **short-session occupancy**, not a rolling-hour regulatory duty-cycle
measurement. A high percentage over a short burst alone does not establish an
hourly-limit violation. The device does not transmit any RF signal.

The calibration trace is a sequential RSSI sweep, not a simultaneous FFT.
It uses the same 101.5625 kHz receive filter as measurement, so adjacent 30 kHz
steps overlap and narrow signals appear broadened. Each retune allows 3 ms
for RSSI settling. Short transmissions may be missed while scanning another
frequency; use repeated signals when calibrating. Sweeping is restricted to
calibration. Armed, recording, and completed sessions remain on the center
frequency, and sweep samples never contribute to measured duty cycle.

The upper activity graph uses a fixed scale from **0s** to the selected session
duration. Its trace grows from the left, leaving future time blank. It retains
the entire session with 100 time intervals, each 10–600 ms long for sessions
of 1–60 seconds. Stopping early preserves this scale and leaves the remainder
blank. The lower graph runs from **-10s** to **now** at 100 ms resolution; before
ten seconds have elapsed, its unrecorded area on the left stays blank.

In both graphs, an interval containing any detected activity is shown as 1 so
brief signals remain visible. This display aggregation does not change the
time-weighted RF percentage or the CSV transition timestamps. Both graphs
freeze when recording stops, and the lower graph's right-hand label changes to
**end**. Both keep updating even if the separate CSV transition buffer fills.

## CSV and measurement limits

Completed sessions are stored under **`apps_data/mc_dutycycle/`** on the SD card,
with a timestamp and tick count in each filename. CSV metadata includes the
actual tuned frequency, threshold, elapsed/on-time milliseconds, sample count,
largest sampling gap, intervals longer than 5 ms, and omitted transition count.
Rows contain `elapsed_ms,rf_active,window_end`:

- The initial row marks detection of the first signal.
- Subsequent rows mark observed threshold crossings.
- `window_end=1` marks a measurement boundary, not an observed RF falling edge.

The worker requests a sample every 1 ms. Actual timestamps determine on-time;
the previous observed state is held until the next sample. Scheduling and radio
RSSI settling introduce uncertainty. **gap** on screen reports the largest
sampling gap; CSV also records the number of gaps longer than 5 ms. Signals
shorter than the sampling/receiver response interval can be missed.

The app buffers up to **2048 transition rows** in RAM and writes them only after
capture stops, so SD operations do not interrupt an active measurement.
If this buffer fills, **LOG FULL** appears and CSV reports omitted rows.
Aggregate on-time continues to be calculated, but the truncated transition log
cannot reconstruct the full capture. **SD ERROR** means the capture could not
be completely saved. Results stay visible until another capture is armed.

Software validation covers timing integration, threshold triggering, stopping,
timestamp wrap, buffer exhaustion, a 50% synthetic signal, preset format and
continuous RSSI settings, and entry-point startup with a simulated internal
radio. Receiver tests cover sweep range, changing modes during a sample,
returning to the center before arming, fixed-frequency capture, and plot
coordinate limits. The worker smoke test checks radio call order and arming
during a retune. Activity-history tests cover short pulses, rolling eviction,
full-session retention and scaling, stopping, timestamp wrap, long sampling
gaps, and CSV buffer exhaustion. User testing on a Flipper Zero confirmed
startup, spectrum calibration, and recording with both activity graphs. The
maximum reported sampling gap was 5 ms. Absolute RF timing and RSSI calibration
still need verification against a reference instrument.
