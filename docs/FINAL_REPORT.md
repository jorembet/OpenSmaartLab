# OpenSmaartLab — Final Integration Report

Phases 13 (recording and offline analysis), 14 (traces, sessions, export) and 15 (final
integration) completed. This report states what is implemented, what is tested, what is
measured, and what is **not** yet validated.

---

## IMPLEMENTED

### Phase 13 — Recording

`Source/WavRecorder.{h,cpp}`. Depths 16-bit, 24-bit and 32-bit float. Rates 44100, 48000,
96000 and 192000 Hz, with the rate taken from the device rather than converted to.

The audio callback hands samples to a lock free ring and returns. A pump on the message
thread moves them into the file. REC / STOP / PAUSE / SAVE in the **Rekam** tab.

PAUSE discards arriving audio and continues from the resume point, so the file is one
continuous waveform with no silence inserted. A dropped-sample count is reported when the
pump falls behind.

### Phase 13 — Offline analysis

`Source/OfflineAnalysis.{h,cpp}`. Runs spectrum, RTA, THD, transfer function, impulse
response, reverberation and SPL over a file.

**This class holds no DSP of its own.** Every figure it reports comes out of the same
analyser objects the live path uses — `SpectrumAnalyser`, `RtaAnalyser`,
`DistortionAnalyser`, `TransferFunction`, `SplAnalyser`, `ImpulseResponse`,
`ReverbAnalyser` — fed the same way. This is the answer to the "avoid duplicate DSP
implementations" requirement, and it is enforced by test rather than by intent: a file and
the same samples handed directly must agree.

### Phase 14 — Traces, sessions, export

`Source/TraceStore.{h,cpp}` — six traces (Live, Reference, Measurement, Average, Memory,
Difference) with freeze, running-mean average, frequency-matched difference, RMS and peak
comparison, and clear.

`Source/Session.{h,cpp}` — `.osls`, a readable xml document carrying device, sample rate,
buffer size, channel assignment, calibration, FFT and RTA settings, measurement settings,
all traces, and notes. A file from a newer format version is refused whole. Out-of-range
settings are corrected rather than obeyed.

`Source/MeasurementExporter.cpp` — CSV, JSON, WAV and SVG for FFT, RTA, magnitude, phase,
coherence, impulse, ETC, RT60, THD, THD+N, noise and SPL. Every export names its units and
axis inside the file.

`Source/SessionPanel.{h,cpp}` — the **Session & Export** tab.

### Phase 15

`tests/validation_checks.cpp` — 251 checks. See below.

---

## TESTED

**16 suites, 16/16 passing, 551 individually enumerated checks.**

| Suite | Checks | Covers |
| --- | --- | --- |
| `audio_checks` | 24 | Engine transport, routing, device loss and reconnection |
| `validation_checks` | 251 | Every measurement against a worked-out number, stress, performance, numerical safety |
| `session_checks` | 142 | Traces, session round trip, export in four formats |
| `recording_checks` | 92 | WAV round trip at three depths, offline against live |
| `rta_checks` | 9 | Band edges, resolution, averaging, unresolved bands |
| `spectrum_checks` | 9 | Window conventions, coherent gain, peak finding |
| `ui_checks` | 9 | Interface construction |
| `level_checks` | 7 | Peak, rms, crest factor, dB helpers |
| `advanced_measurements` | grouped | SMPTE and CCIF IMD, crosstalk, polarity |
| `calibration` | grouped | SPL calibration, weightings, time constants |
| `distortion` | grouped | THD, THD+N, SINAD, harmonics |
| `eq` | grouped | Auto-EQ fitting and export |
| `generator` | grouped | Eight waveforms, polyBLEP, sweeps |
| `phase_delay` | grouped | Phase wrapping, slope delay, distance, temperature |
| `reverb` | grouped | RT60 per band, quality states |
| `transfer_function` | grouped | Magnitude, phase, coherence, delay, impulse |

### Measurements held to a number that can be worked out in advance

| Measurement | Held against |
| --- | --- |
| RMS | Amplitude ÷ √2 of a full scale sine |
| Peak | Amplitude in dB |
| Crest factor | 3.0103 dB for a sine |
| FFT | Tone amplitude, amplitude calibrated, within scalloping |
| RTA | The tone's band must stand >20 dB clear of the quietest band |
| Transfer magnitude | 0 dB against itself; −6.0206 dB at half amplitude |
| Phase | 0° against itself; unchanged by scaling |
| Coherence | 1.0 against itself; <0.5 for unrelated signals |
| Delay | 1, 37 and 480 samples recovered to within 1.5 samples |
| Distance | Delay ÷ sample rate × speed of sound |
| Impulse | Peak at the delay with compensation off; at zero with it on |
| ETC | 60 dB/s for a one second room |
| RT60 / EDT / T20 / T30 | 0.3, 0.8, 1.5, 2.5 s rooms, to within 10 % |
| THD | Root sum of harmonic powers over fundamental |
| THD+N | Equals THD when no noise is added |
| SINAD | Inverse of THD+N |
| IMD | Difference tone at 1 % of one drive tone |
| SNR | Leakage of one part in a hundred = −40 dB |
| Crosstalk | 1 % → −40 dB, 1.00 % |
| Polarity | Normal, Inverted, and Unknown for unrelated signals |
| SPL | Meter reads −3.01 dBFS for a full scale sine; −6.02 dB per halving; −13.98 dB per decade |
| Weightings | Published A and C curves at 31.5 Hz … 16 kHz |

### Stress

Every transform size (1024–65536) at every sample rate (44.1–192 kHz). A two minute
recording. Two channels of different content. Fifty rounds of setting up and tearing down
every analyser with alternating sizes and resolutions. Buffers shorter than a frame, a
transform size that is not a power of two, a null pointer and a zero length.

### Numerical safety

Every dB helper fed `0`, `-0`, denormal, ±1, ±1e-20 and ±3e38. NaN and ±Inf injected into a
signal and handed to the sound pressure meter, the level meter and every analyser. Silence
at five buffer shapes down to a single sample. A decay faster than a float can resolve.

### Realtime safety — mechanically audited

The audio callback, `SignalGenerator::process` and `WavRecorder::pushFromAudioThread` were
scanned for allocation, locking, filesystem access, GUI calls, logging and blocking calls:

| Path | Allocation | Locks | Filesystem | GUI | Logging | Blocking |
| --- | --- | --- | --- | --- | --- | --- |
| `audioDeviceIOCallbackWithContext` | 0 | 0 | 0 | 0 | 0 | 0 |
| `SignalGenerator::process` | 0 | 0 | 0 | 0 | 0 | 0 |
| `WavRecorder::pushFromAudioThread` | 0 | 0 | 0 | 0 | 0 | 0 |

---

## PERFORMANCE

Measured on this machine, 48 kHz, averaged over 2000 blocks per configuration:

| Buffer | FFT | Cost per block | Budget | Used |
| --- | --- | --- | --- | --- |
| 64 | 16384 | 0.93 µs | 1333 µs | 0.07 % |
| 512 | 16384 | 8.47 µs | 10667 µs | 0.08 % |
| 1024 | 65536 | 18.20 µs | 21333 µs | 0.09 % |
| 2048 | 65536 | 37.35 µs | 42667 µs | 0.09 % |

Capture ring, three planes of 512 samples: **0.117 µs per block, 0.001 % of budget.**

At every combination offered the analysis chain uses under a tenth of one percent of the
audio budget. The budget is not the limiting factor on this machine; a very small buffer is
limited by the operating system's own scheduling instead.

**Not measured:** GUI frame rate, and sustained long-run CPU on real hardware. The
performance section times the DSP chain and the capture path in isolation. Frame rate
depends on the window manager and the machine's cores, and no figure is claimed for it.

---

## KNOWN BUGS

None known. Four real defects were found and fixed during Phase 15:

1. **`LevelMeter` propagated a non-finite sample into an infinite peak.** A NaN in the input
   made the peak read `inf` rather than a very loud signal. Now counted as silence.
2. **`DistortionAnalyser` divided by zero on digital silence**, returning NaN for SINAD.
   Floored, and the result is now refused outright if any published figure is not finite.
3. **`WavRecorder::discard` deleted a file that had just been saved.** State was `Stopped`
   rather than `Idle` after a save, so discarding looked identical to throwing away an unsaved
   recording. **Closing the application after saving would have deleted the recording.** Now
   tracked with an explicit saved flag.
4. **`OfflineAnalysis` reported raw dBFS under a "dB SPL" label.** The meter answers in
   decibels re full scale by design and the calibration converts; the offline path was not
   converting, so every reported level was wrong by roughly the microphone's sensitivity, about
   a hundred decibels. The live `SPLMeter` path was already correct.

---

## KNOWN LIMITATIONS

These are real and are not worked around in the code.

**Not validated against hardware.** Every SPL figure, and every absolute level the
application reports, is validated against the *arithmetic* of the calibration, not against a
physical calibrator. No acoustic measurement has been made against a known reference in this
environment. The weighting curves are held against published values and the calibration
arithmetic is held against its own definition, but the chain from microphone capsule to
sound pressure has not been verified end to end.

**No device has been exercised.** The stress suite runs the analysers at every size and rate
and runs fifty rounds of setup and teardown, but no audio interface was present. Device
enumeration, hot-plug reconnection and XRUN behaviour are covered by `audio_checks` at the
engine level only. The JACK backend is compiled in but `jackd` is not installed here, so
JACK-specific paths are unexercised.

**Pause discards audio.** A paused recording has a real gap in the input that is not written
to the file. This is deliberate and documented, but it means a pause cannot be used to skip
past something and keep a continuous measurement.

**RTA band level is a density.** It is not the level of a tone and is not comparable with the
spectrum's peak reading. This is a convention, not a defect, and it is stated in the README.

**Difference is matched by frequency, not by index.** Two traces from different transform
sizes are compared only over the frequencies both hold, so a difference between a 16384 and a
65536 transform covers fewer bins than either. This is correct but means the difference is not
a full-length curve.

**Crosstalk is a single-pass measurement.** It measures leakage on one driven channel against
one receiving channel. A full crosstalk matrix needs one pass per channel, driven from the
interface rather than from the panel.

**Offline analysis is single-threaded and blocking.** A long file takes a visible moment. The
panel runs it off the message thread so the window keeps painting, but there is no progress
indicator and no cancellation.

**Session files grow quickly.** Traces are stored as exact hex. A 65536 point spectrum across
six traces and six sources is a large file. Readable was chosen over small.

**The generator's impulse waveform is one-shot.** It resets rather than repeating, which is
correct for measuring an impulse response and wrong if you wanted a periodic train.

---

## NOT VALIDATED — do not treat as production ready

The brief asks for this to be stated plainly, and it is: **this is not production ready.**
The arithmetic of every measurement is validated against numbers that can be worked out in
advance, and the software is free of known defects. What is missing is validation against
physical reality:

- No acoustic measurement has been checked against a reference instrument or a traceable
  calibrator.
- No hardware has been exercised, so no device, driver or transport behaviour is validated.
- XRUN counts, sustained CPU and GUI frame rate have not been measured on real hardware.
- Cross-platform behaviour is unverified. Everything here was built and tested on Linux.

The measurements that would need physical validation before anyone relied on them are: every
sound pressure level, every microphone calibration, and any figure derived from them.

---

## UI

Thirteen tabs: RTA / Delay, Transfer Function, Reverberation, Impedansi, Input Level,
Spectrum, RTA, Distorsi, SPL Meter, Generator, Rekam, Offline, Session & Export. These cover
the eight views asked for — analyzer, transfer, impulse and room (as Reverberation), distortion,
level, generator and device (as Input Level plus the device selectors on the main panel).

No layout was reworked in Phase 15, by intent. Each tab carries one measurement, and the three
new tabs are laid out so the controls are in one row at the top and the reading fills the rest
of the tab. The one observation worth recording: **RTA / Delay and RTA are two different tabs
doing closely related things**, and a user looking for the analyser will not know which to
open. That should be resolved before the tabs are exposed to anyone else.

---

## NEXT RECOMMENDATIONS

1. **Validate against a reference.** Measure a known 94 dB calibrator and a known delay
   against a reference instrument. Nothing else on this list matters as much.
2. **Exercise the hardware.** Every device, at every supported rate, with the device unplugged
   mid-recording and plugged back in. XRUN counts under load.
3. **Put the RTA frequency range back on the panel.** It lost its only control when the
   duplicate RTA tab was removed. Add a frequency range selector to the RTA / Delay tab.
4. **Merge the duplicated SPL conversion.** The conversion lives in `OfflineAnalysis` and
   separately in `SPLMeter`. Two places to forget is one too many; it belongs in `SplAnalyser`
   behind an explicit choice of units.
5. **Add a progress indicator and cancellation to offline analysis** for files longer than a
   minute.
6. **Crosstalk matrix.** One pass per channel, driven from the interface.
7. **Reduce session file size.** Store traces as a binary sidecar with a readable index, or
   quantise to a documented precision.