"""Synthetic full-duplex tests: no audio hardware or playback needed."""
import sys
import types
import unittest
from unittest.mock import patch
import numpy as np


class FakeStream:
    instances = []
    def __init__(self, **kwargs):
        self.kwargs = kwargs
        self.closed = False
        self.instances.append(self)
    def start(self):
        pass
    def stop(self):
        pass
    def close(self):
        self.closed = True


fake_sd = types.SimpleNamespace(InputStream=FakeStream, OutputStream=FakeStream,
                                Stream=FakeStream, query_devices=lambda *args: {"max_output_channels": 2})
with patch.dict(sys.modules, {"sounddevice": fake_sd}):
    from audio_engine import AudioEngine
from dsp import find_delay


class GeneratorReferenceTests(unittest.TestCase):
    def test_mono_microphone_gets_reference_and_measurable_delay(self):
        engine = AudioEngine()
        engine.start_input(0, 48000, 1)
        original_input = engine.stream
        engine.start_generator(1, "Pink Noise")
        self.assertTrue(original_input.closed)
        self.assertTrue(engine.duplex)
        self.assertEqual(engine.stream.kwargs["device"], (0, 1))
        callback = engine.stream.kwargs["callback"]
        played = []
        for block in range(110):
            mic = np.zeros((1024, 1), np.float32)
            if block >= 3:
                mic[:, 0] = np.asarray(played[(block-3)*1024:(block-2)*1024]) * .4
            output = np.zeros((1024, 2), np.float32)
            callback(mic, output, 1024, None, None)
            np.testing.assert_array_equal(output[:, 0], output[:, 1])
            played.extend(output[:, 0])
        mic, ref = engine.get_latest_pair(65536)
        ms, lag = find_delay(ref, mic[:, 0], 48000)
        self.assertEqual(lag, 3072)
        self.assertEqual(ms, 64)
        engine.stop_generator()
        self.assertFalse(engine.duplex)
        self.assertFalse(engine.gen_on)
        self.assertIsNotNone(engine.stream)
        self.assertNotIn("output", engine.stream.kwargs)
        engine.close()

    def test_ring_wrap_keeps_reference_aligned(self):
        engine = AudioEngine(samplerate=1000, channels=1, seconds=1)
        for offset in range(0, 3500, 500):
            values = np.arange(offset, offset+500, dtype=np.float32)
            engine._capture(values[:, None], values * 2)
        mic, ref = engine.get_latest_pair(1000)
        np.testing.assert_array_equal(mic[:, 0], np.arange(2500, 3500))
        np.testing.assert_array_equal(ref, mic[:, 0] * 2)

    def test_duplex_open_failure_restores_microphone(self):
        engine = AudioEngine(channels=1)
        engine.start_input(0)
        with patch.object(fake_sd, "Stream", side_effect=RuntimeError("unsupported device pair")):
            with self.assertRaisesRegex(RuntimeError, "unsupported device pair"):
                engine.start_generator(1)
        self.assertFalse(engine.gen_on)
        self.assertFalse(engine.duplex)
        self.assertIsNotNone(engine.stream)
        engine.close()


if __name__ == "__main__":
    unittest.main()
