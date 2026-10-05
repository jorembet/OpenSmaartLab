# OpenSmaartLab 1.1.0

Real-time acoustic measurement application: RTA with octave bands, dual-channel transfer
function with magnitude/phase/coherence, reverberation, SPL meter, signal generator, and
in this release spectrum analysis, RT60, distortion, impedance, WAV recording, offline
analysis over a file, and sessions with export.

## Downloads

| File | Platform | Notes |
| --- | --- | --- |
| `OpenSmaartLab-1.1.0-linux-x86_64.tar.gz` | Linux x86-64 | Built on Ubuntu 26.04, needs glibc 2.43+ |
| `Smaart8Setup.exe` | Windows | WiX bootstrapper chaining an MSI |
| `Source code (zip)` / `Source code (tar.gz)` | — | Tag `v1.1.0` |

There is no macOS build. The C++/JUCE source builds on all three; see the README for the
build steps. The Python implementation in the repository runs on Windows, macOS and Linux
without compiling.

## Install on Linux

```bash
tar xzf OpenSmaartLab-1.1.0-linux-x86_64.tar.gz
cd OpenSmaartLab-1.1.0-linux-x86_64
chmod +x OpenSmaartLab
./OpenSmaartLab
```

If the window does not open, install the runtime libraries:

```bash
sudo apt install libgtk-3-0 libasound2t64 libfreetype6 libpng16-16 libbrotli1 zlib1g
```

The binary needs glibc 2.43 or newer. On older distributions, build from source instead.

## Install on Windows

Run `Smaart8Setup.exe` and follow the prompts.

## What is new since 1.0.0

- **Transfer Function** tab: magnitude, phase and coherence of one dual-channel run
- **Impedansi** tab: sweep driven impedance measurement
- **Input Level** tab: per channel level, peak and crest factor
- **Spectrum** tab: single frame spectrum with a peak read-out
- **Distorsi** tab: THD, THD+N, SINAD and the harmonic table of a tone
- **Rekam** tab: WAV recording at 16-bit, 24-bit and 32-bit float, 44.1 kHz to 192 kHz,
  with REC / STOP / PAUSE / SAVE
- **Offline** tab: every measurement run over a recorded file, through the same analyser
  objects as the live path
- **Session & Export** tab: six traces with freeze and averaging, `.osls` session files,
  and export to CSV, JSON, WAV and SVG
- **Spectrogram** on the RTA tab: a scrolling heat map of the measurement history, where
  time runs upwards
- RT60 per band, and reverb quality states that say how far to trust the estimate
- Pink noise band-limited to 20 Hz - 20 kHz, with the range adjustable from the UI
- Generator frequency can be typed exactly, and the sweep and noise levels are adjustable
- Two output channels, so the left/right routing has something to separate
- Per-driver, per-channel measurement, with delay compensation applied before averaging
- Tests grown from 2 suites to 18, covering the added measurements; see
  `docs/FINAL_REPORT.md`

## Fixed in this release

- The spectrogram now reports the level between FFT bins for bands narrower than the bin
  spacing, instead of leaving holes where no bin fell
- Spectrogram history is kept while the view is behind a tab, rather than being dropped
  the moment the window loses focus

## What it does

- Real-time FFT and octave-band RTA, 1/1 up to 1/12 octave, with ISO 266 nominal
  frequency labels
- Linear bar view with 100 evenly spaced bars
- Transfer function magnitude, phase and coherence, with automatic delay compensation
- Reverberation: impulse response, energy decay curve, Schroeder decay, EDT / RT20 /
  RT30 / RT60 per band
- SPL meter with A / C / Z weighting, peak and Leq, and microphone calibration
- Signal generator: pink noise, white noise, sine and sweep, with driver bands
- WAV recording and offline analysis over a recorded file
- Sessions and export to CSV, JSON, WAV and SVG

## Not validated

These are real and are documented rather than worked around:

- **No absolute level has been checked against a physical calibrator.** Every SPL figure
  is held against the arithmetic of the calibration, not against a known reference. The
  chain from microphone capsule to sound pressure is not verified end to end.
- **No audio interface was available while this was built.** Device enumeration, hot-plug
  reconnection and XRUN behaviour are covered at the engine level only.
- **Pause discards audio.** A paused recording has a real gap in the input that is not
  written to the file.
- **RTA band level is a density.** It is not the level of a tone and is not comparable
  with the spectrum's peak reading.

Bluetooth output goes through PipeWire and encodes to SBC, so it is not appropriate for
level accuracy. Use analogue or USB output for measurement work.

Independent implementation. Not affiliated with or endorsed by Rational Acoustics.