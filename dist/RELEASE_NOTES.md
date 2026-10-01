# OpenSmaartLab 1.0.0

Real-time acoustic measurement application: RTA, dual-channel transfer function with
magnitude/phase/coherence, reverberation, SPL meter, and signal generator.

## Downloads

| File | Platform | Notes |
| --- | --- | --- |
| `OpenSmaartLab-1.0.0-linux-x86_64.tar.gz` | Linux x86-64 | Built on Ubuntu 26.04, needs glibc 2.43+ |
| `Source code (zip)` / `Source code (tar.gz)` | — | Tag `v1.0.0` |

There is no Windows or macOS build in this release. The C++/JUCE source builds on all
three; see the README for the build steps. The Python implementation in the repository
runs on Windows, macOS and Linux without compiling.

## Install on Linux

```bash
tar xzf OpenSmaartLab-1.0.0-linux-x86_64.tar.gz
cd OpenSmaartLab-1.0.0-linux-x86_64
chmod +x OpenSmaartLab
./OpenSmaartLab
```

If the window does not open, install the runtime libraries:

```bash
sudo apt install libgtk-3-0 libasound2t64 libfreetype6 libpng16-16 libbrotli1 zlib1g
```

The binary needs glibc 2.43 or newer. On older distributions, build from source
instead.

## What it does

- Real-time FFT and octave-band RTA, 1/1 up to 1/12 octave, with ISO 266 nominal
  frequency labels
- Linear bar view with 100 evenly spaced bars
- Transfer function magnitude, phase and coherence
- Live impulse response with EDT, RT20, RT30 and RT60
- Delay finder using normalised cross-correlation
- SPL meter with A/C/Z weighting, peak and Leq
- Pink noise, white noise, sine and sweep generator
- Microphone calibration that reads Dayton Audio `.txt` calibration files
- Named RTA snapshots that can be saved and reloaded for comparison

## Measurement setup

For transfer function and delay:

- Output: pink noise to the system under test
- Reference input: electrical loop or mixer reference
- Measurement input: measurement microphone

Both inputs must be on the same audio interface. A microphone on its own is enough for
RTA.

## Known limitations

- Bluetooth output routes through PipeWire and changes the system default sink while
  running. A2DP encodes to SBC, so use analogue or USB output for level accuracy.
- Uncalibrated readings are in dBFS, not dB SPL. Load the microphone calibration file
  or apply an SPL calibrator before treating readings as SPL.
- No Windows or macOS binary is published for this release.

## Independence

This is an independent implementation. It is not affiliated with or endorsed by Rational
Acoustics and contains no third-party Smaart source code, artwork, or algorithms.

MIT licensed. See LICENSE.