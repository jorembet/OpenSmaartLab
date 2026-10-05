"""Automatic delay search on the real window: no audio hardware or display needed."""
import os
import sys
import types
import unittest
from unittest.mock import patch

import numpy as np

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from PySide6 import QtWidgets
from dsp import find_delay  # loads numpy and scipy before the sounddevice patch


class FakeStream:
    def __init__(self, **kwargs):
        self.kwargs = kwargs
    def start(self):
        pass
    def stop(self):
        pass
    def close(self):
        pass


def query_devices(*args):
    return [dict(name="Fake Interface", max_input_channels=2, max_output_channels=2)]


fake_sd = types.SimpleNamespace(InputStream=FakeStream, OutputStream=FakeStream,
                                Stream=FakeStream, query_devices=query_devices)
with patch.dict(sys.modules, {"sounddevice": fake_sd}):
    from main import MainWindow, DELAY_HYSTERESIS_SAMPLES

FS = 48000
LAG = 480


def delayed_pair(lag, n=16384, seed=7):
    rng = np.random.default_rng(seed)
    ref = rng.normal(size=n + 2*lag)
    meas = np.concatenate([np.zeros(lag), ref[:-lag]])
    return meas, ref


class AutoDelayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])

    def setUp(self):
        self.win = MainWindow()
        self.win.engine.start_input(0, FS, 2)
        self.win.reset_routing()

    def tearDown(self):
        self.win.engine.close()
        self.win.close()

    def feed(self, meas, ref):
        self.win.engine._capture(np.column_stack([meas, ref]).astype(np.float32))

    def feed_duplex(self, lag, n=70000, seed=5, gen_type="Pink Noise"):
        rng = np.random.default_rng(seed)
        generated = rng.normal(size=n + 2*lag)*0.05
        mic = np.concatenate([np.zeros(lag), generated[:-lag]])
        mic = mic + rng.normal(size=len(mic))*0.0005
        engine = self.win.engine
        engine.gen_on, engine.duplex, engine.gen_type = True, True, gen_type
        engine._capture(np.column_stack([mic, mic]).astype(np.float32), generated.astype(np.float32))

    def search_now(self):
        self.win.last_delay_check = 0.0
        self.win.auto_delay_check()

    def test_external_reference_is_searched_without_pressing_the_button(self):
        meas, ref = delayed_pair(LAG)
        self.feed(meas, ref)
        self.win.update_measurements()
        self.assertEqual(self.win.delay_samples, LAG)
        self.assertAlmostEqual(self.win.delay_samples/FS*1000, 10.0, places=6)
        self.assertIn("(auto)", self.win.delay_label.text())
        self.assertTrue(self.win.auto_delay_cb.isChecked())

    def test_found_delay_is_compensated_before_averaging(self):
        meas, ref = delayed_pair(LAG)
        self.feed(meas, ref)
        self.win.update_measurements()
        data = self.win.last_data
        self.assertEqual(find_delay(ref[-16384:], meas[-16384:], FS)[1], LAG)
        band = (data["freq"] >= 500) & (data["freq"] <= 2000)
        self.assertTrue(np.all(np.abs(data["mag"][band]) < 3),
                        "delay compensation should flatten a pure delay path")
        self.assertLess(int(np.argmax(np.abs(data["ir"]))), 32)

    def test_small_jitter_keeps_the_delay_and_the_average(self):
        meas, ref = delayed_pair(LAG)
        self.feed(meas, ref)
        self.win.update_measurements()
        self.assertEqual(self.win.delay_samples, LAG)
        averaged = self.win.tf.Sxx
        self.assertIsNotNone(averaged)
        self.feed(*delayed_pair(LAG + DELAY_HYSTERESIS_SAMPLES, seed=9))
        self.search_now()
        self.assertEqual(self.win.delay_samples, LAG)
        self.assertIs(self.win.tf.Sxx, averaged)

    def test_real_change_resets_the_average(self):
        meas, ref = delayed_pair(LAG)
        self.feed(meas, ref)
        self.win.update_measurements()
        self.feed(*delayed_pair(LAG + 240, seed=11))
        self.search_now()
        self.assertEqual(self.win.delay_samples, LAG + 240)
        self.assertIsNone(self.win.tf.Sxx)

    def test_turning_auto_off_stops_the_search(self):
        meas, ref = delayed_pair(LAG)
        self.feed(meas, ref)
        self.win.auto_delay_cb.setChecked(False)
        self.win.update_measurements()
        self.assertEqual(self.win.delay_samples, 0)
        self.assertNotIn("(auto)", self.win.delay_label.text())

    def test_quiet_search_keeps_the_last_reading_when_the_reference_dies(self):
        meas, ref = delayed_pair(LAG)
        self.feed(meas, ref)
        self.win.update_measurements()
        label = self.win.delay_label.text()
        self.feed(np.zeros(16384), np.zeros(16384))
        self.search_now()
        self.assertEqual(self.win.delay_label.text(), label)

    def test_button_search_reports_without_the_auto_marker(self):
        meas, ref = delayed_pair(LAG)
        self.feed(meas, ref)
        self.win.request_delay()
        self.assertEqual(self.win.delay_samples, LAG)
        self.assertNotIn("(auto)", self.win.delay_label.text())
        self.assertIn("Delay", self.win.delay_label.text())

    def test_generator_reference_gives_the_total_loop_delay(self):
        lag = 1440
        self.feed_duplex(lag)
        self.win.update_measurements()
        self.assertEqual(self.win.delay_samples, lag)
        self.assertIn("Delay total", self.win.delay_label.text())
        self.assertLess(int(np.argmax(np.abs(self.win.last_data["ir"]))), 32)

    def test_auto_search_skips_a_generator_signal_that_is_not_noise(self):
        self.feed_duplex(LAG, gen_type="Sine")
        self.win.update_measurements()
        self.assertEqual(self.win.delay_samples, 0)
        self.assertEqual(self.win.delay_label.text(), "Delay: -- ms")


if __name__ == "__main__":
    unittest.main()