# OpenSmaartLab

Independent real-time acoustic measurement application, built around the workflow of
professional dual-channel FFT analysers.

Two implementations live in this repository:

| Folder | Stack | Purpose |
| --- | --- | --- |
| `main.py` | Python, PySide6 + pyqtgraph | Reference implementation, cross-platform |
| `JUCE/` | C++17, JUCE 7.0.12 | Native desktop build, lower latency |

The two apps cover the same measurement scope. The C++ build is the one to use for
serious work: it is the only one that can keep a two-channel measurement stable under
load.

---

## Features

### Analysis

- Real-time FFT / RTA
- Spectrograph
- Dual-channel Transfer Function
  - Magnitude
  - Phase
  - Coherence
- Live impulse response derived from the transfer function
- Delay finder using normalised cross-correlation
- Reverberation tab
  - Linear IR
  - ETC
  - Schroeder decay
  - EDT / RT20 / RT30 / RT60 estimate
- SPL meter
  - A / C / Z weighting
  - Fast / Slow
  - Peak
  - Leq
  - Calibration offset
- Channel assignment
  - Measurement and reference are assigned to the left and right input separately, so a
    microphone on one input and a loopback tap on the other is a normal wiring
  - The generator can feed one output side only, leaving the other silent
  - The plot legend names the channel behind each trace
- Signal generator
  - Pink noise
  - White noise
  - Sine
  - Sweep
  - Driver bands for pink noise: subwoofer, woofer, midrange, tweeter, or the full
    20 Hz - 20 kHz band. Room correction is done one driver at a time, because
    coherence and level are only meaningful inside the band a driver reproduces
  - Output side: left, right or both, so an amplifier can be fed from one output while
    the reference tap stays separate
  - Frequency and both band limits can be typed exactly, next to their sliders
  - The spectrum plot follows the settings: the axis zooms onto the selected band,
    tone or sweep range, the band edges are shaded, and a sweep shows where it is

### Transfer Function tab

- Magnitude, phase and coherence of one dual channel measurement, stacked on a shared
  frequency axis
- Coherence is computed only where the reference carries signal. Bins outside the
  excited band are blanked, because their coherence is the ratio of two near-zero
  powers and used to read as a confident 100 %
- The average coherence is weighted by reference power, so it describes the band being
  measured instead of being averaged over the whole axis
- A validity line at 80 % coherence, because below that the measurement is not usable
  however good the curve looks
- **Cari Delay** searches for the first impulse peak once and restarts the averages, so
  every averaged frame shares one delay compensation
- Coherent regions are smoothed over a third of an octave, matching the rest of the app

### RTA display

- Two styles: octave bands, or 100 evenly spaced bars on a linear frequency axis
- Band resolution from 1/1 octave up to 1/12 octave
- Transfer function and coherence traces break where the reference carried no signal
- Band centres follow the ISO 266 nominal frequency table (20 Hz, 25, 31.5, 40, …)
- Every band is labelled with its frequency
- Bars tile the plot exactly: no gaps, no clipping at 20 Hz or 20 kHz
- Peak hold and freeze
- Per-trace colour picker, remembered between sessions
- DC offset and sub-audio content are excluded from the lowest band, so a USB
  interface sitting a few millivolts off zero does not read as a loud low band

### Microphone calibration

- Sensitivity conversion from dBFS to dB SPL
- Frequency response correction, interpolated per FFT bin
- Reads Dayton Audio `.txt` calibration files, including the per-serial-number
  sensitivity line (`*1000Hz<TAB>-38.5`)
- Off by default. Calibration is opt-in from the **Kalibrasi Mic** menu, because
  switching to dB SPL moves the trace outside the default display window

### Capture

- Save the current RTA under a name, with the calibration note embedded
- Load it back later to compare against live input
- CSV export of the raw spectrum

---

## Build

### C++ / JUCE

Requires CMake 3.22+, a C++17 compiler, and the JUCE dependencies, which CMake
fetches automatically.

```bash
cd JUCE
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/OpenSmaartLab_artefacts/Release/OpenSmaartLab
```

On Debian/Ubuntu the GUI backend needs GTK development headers:

```bash
sudo apt install libgtk-3-dev libasound2-dev libcurl4-openssl-dev
```

### Python

Ubuntu / Mint:

```bash
sudo apt install python3-venv libportaudio2 portaudio19-dev
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python main.py
```

Windows:

```powershell
py -m venv .venv
.venv\Scripts\activate
pip install -r requirements.txt
python main.py
```

ASIO support depends on the PortAudio build and the installed interface driver.

---

## Tests

```bash
cd JUCE/build
ctest --output-on-failure
```

`audio_checks` covers the DSP path: pink-noise coherence, channel copy, delay
detection, driver band presets, band tables, DC rejection, calibration parsing, and
the snapshot round trip. `ui_checks` renders the panels to PNG and checks the
generator controls, including that a driver band reaches the generator and that a
hand set band reports itself as manual. Both run under `xvfb-run`, since the checks
need a display server for the GUI target.

Python:

```bash
source .venv/bin/activate
python -m unittest discover -s tests
```

---

## Measurement wiring

For a transfer-function measurement:

- **Reference input**: direct electrical loop or mixer reference
- **Measurement input**: measurement microphone
- **Output**: pink noise to the system under test

Set REF and MEAS channels in the toolbar, then start.

Delay needs two inputs captured simultaneously on the same interface: one
microphone and one electrical reference carrying the same signal. Use broadband
noise. Positive delay means the microphone signal arrives later than the reference.
Without a usable reference the reading shows `--`, and a microphone on its own still
supports RTA. This measures relative arrival time, including differences between the
signal paths, not speaker distance by itself.

---

## SPL calibration

With an acoustic calibrator (for example 94 dB @ 1 kHz):

1. Place the calibrator on the measurement microphone.
2. Open the SPL Meter tab.
3. Adjust the calibration offset until the meter reads the calibrator level.

Software-only SPL values are not standards-compliant until the complete
microphone and interface chain is calibrated.

For the RTA, use **Kalibrasi Mic** to load the microphone's own calibration file
instead of a single offset.

---

## Notes

This project is an independent implementation. It does not contain Rational
Acoustics source code, artwork, trademarks, proprietary algorithms, or licensed
Smaart assets.

Bluetooth output goes through PipeWire: the C++ build enumerates sinks with
`pactl` and routes playback through the ALSA `pipewire` PCM, which means it changes
the system default sink while a measurement is running. Bluetooth (A2DP) also
encodes to SBC, so it is not appropriate for level accuracy. Use analogue or USB
output for measurement work.