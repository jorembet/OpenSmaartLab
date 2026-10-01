"""Readable audio labels: friendly names instead of raw ALSA descriptions."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from device_labels import device_label, friendly_device_name


class LabelTests(unittest.TestCase):
    def test_no_raw_alsa_text(self):
        raw = ["Rate Converter Plugin Using Libav/FFmpeg Library",
               "Plugin using Speex DSP (resample, agc, denoise, echo, dereverb)",
               "Plugin for channel downmix (stereo) with a simple spacialization",
               "sof-hda-dsp, ; Direct hardware device without any conversions (1)",
               "PipeWire Sound Server", "PulseAudio Sound Server", "Default Sink", "default"]
        for name in raw:
            label = device_label(name, False, 2)
            for bad in ("Rate Converter", "Speex DSP", "spacialization", "Sound Server",
                        "Direct hardware", ", ;", "Direct sample mixing"):
                self.assertNotIn(bad, label, f"{bad!r} leaked into {label!r}")

    def test_known_mappings(self):
        cases = {
            "bluez_output.XY.1": "Speaker Bluetooth",
            "bluez_input.XY.2": "Mic Bluetooth",
            "default": "Sistem (audio bawaan OS)",
            "pulse": "Sistem (PipeWire)",
            "dmix": "Kartu suara (mix stereo)",
            "upmix": "Kartu suara (campuran kanal)",
            "lavrate": "Kartu suara (resampler)",
            "speex": "Mic (resample + pengurangan noise)",
        }
        for raw, expected in cases.items():
            self.assertEqual(friendly_device_name(raw), expected, raw)

    def test_route_names(self):
        self.assertEqual(
            friendly_device_name("alsa_output.pci-0000_00_1f.3-platform-skl_hda_dsp_generic.HiFi__Speaker__sink"),
            "Speaker")
        self.assertEqual(
            friendly_device_name("alsa_output.pci-0000_00_1f.3-platform-skl_hda_dsp_generic.HiFi__Speaker__sink.monitor"),
            "Monitor Speaker")

    def test_kinds(self):
        self.assertTrue(device_label("sof-hda-dsp: Analog Stereo (hw:0,0)", True, 2).startswith("Line in"))
        self.assertTrue(device_label("Built-in Speakers", False, 2).startswith("Speaker"))
        self.assertTrue(device_label("USB Headset", False, 2).startswith("Headphone"))
        self.assertTrue(device_label("bluez_output.XY.1", False, 2).startswith("Output audio Bluetooth"))
        self.assertTrue(device_label("USB Audio", False, 2).startswith("Output audio USB"))

if __name__ == "__main__":
    unittest.main()
