import threading
import numpy as np
import sounddevice as sd
from dsp import pink_noise


class AudioEngine:
    def __init__(self, samplerate=48000, channels=2, seconds=8):
        self.fs = samplerate
        self.channels = channels
        self.seconds = seconds
        self.capacity = samplerate * seconds
        self.lock = threading.Lock()
        self.buffer = np.zeros((self.capacity, channels), dtype=np.float32)
        self.reference = np.zeros(self.capacity, dtype=np.float32)
        self.write_pos = self.filled = 0
        self.stream = self.out_stream = None
        self.input_device = self.output_device = None
        self.duplex = False
        self.gen_on = False
        self.gen_type = "Pink"
        self.gen_level = 0.05
        self.gen_freq = 1000.0
        self.phase = self.sweep_phase = 0.0
        self.rng = np.random.default_rng()

    @staticmethod
    def devices():
        return sd.query_devices()

    def _reset_capture(self):
        with self.lock:
            self.capacity = self.fs * self.seconds
            self.buffer = np.zeros((self.capacity, self.channels), dtype=np.float32)
            self.reference = np.zeros(self.capacity, dtype=np.float32)
            self.write_pos = self.filled = 0

    def _capture(self, indata, reference=None):
        # Both arrays come from the same full-duplex callback/sample interval.
        with self.lock:
            data = indata[-self.capacity:, :self.channels]
            ref = np.zeros(len(data), dtype=np.float32) if reference is None else reference[-len(data):]
            n = len(data)
            first = min(n, self.capacity - self.write_pos)
            self.buffer[self.write_pos:self.write_pos + first] = data[:first]
            self.reference[self.write_pos:self.write_pos + first] = ref[:first]
            if first < n:
                self.buffer[:n-first] = data[first:]
                self.reference[:n-first] = ref[first:]
            self.write_pos = (self.write_pos + n) % self.capacity
            self.filled = min(self.capacity, self.filled + n)

    def start_input(self, device=None, samplerate=None, channels=None):
        self.stop_generator(resume_input=False)
        self.stop_input()
        if samplerate:
            self.fs = int(samplerate)
        if channels:
            self.channels = int(channels)
        self.input_device = device
        self._reset_capture()

        def callback(indata, frames, time, status):
            self._capture(indata)

        self.stream = sd.InputStream(device=device, channels=self.channels, samplerate=self.fs,
                                     callback=callback, blocksize=1024, dtype="float32")
        try:
            self.stream.start()
        except Exception:
            self.stop_input()
            raise

    def stop_input(self):
        self.duplex = False
        if self.stream:
            stream, self.stream = self.stream, None
            try:
                stream.stop()
            finally:
                stream.close()

    def get_latest_pair(self, n):
        with self.lock:
            n = min(int(n), self.filled)
            if n <= 0:
                return np.zeros((0, self.channels), dtype=np.float32), np.zeros(0, dtype=np.float32)
            indices = (np.arange(n) + self.write_pos - n) % self.capacity
            return self.buffer[indices].copy(), self.reference[indices].copy()

    def get_latest(self, n):
        return self.get_latest_pair(n)[0]

    def _generate(self, frames):
        if not self.gen_on:
            return np.zeros(frames, dtype=np.float32)
        t = np.arange(frames) / self.fs
        if self.gen_type == "Sine":
            ph = self.phase + 2*np.pi*self.gen_freq*t
            y = np.sin(ph)
            self.phase = (ph[-1] + 2*np.pi*self.gen_freq/self.fs) % (2*np.pi)
        elif self.gen_type in ("White", "White Noise"):
            y = self.rng.normal(0, 0.25, frames)
        elif self.gen_type == "Sweep":
            idx = (np.arange(frames) + self.sweep_phase) % (self.fs*5)
            inst = 20 * (1000**(idx/(self.fs*5)))
            ph = self.phase + 2*np.pi*np.cumsum(inst)/self.fs
            y = np.sin(ph)
            self.phase = ph[-1] % (2*np.pi)
            self.sweep_phase = (self.sweep_phase+frames) % (self.fs*5)
        else:
            y = pink_noise(frames, self.rng)
        return np.clip(y*self.gen_level, -0.99, 0.99).astype(np.float32)

    def start_generator(self, device=None, kind="Pink", level_db=-24, freq=1000):
        self.stop_generator()
        capture_active = self.stream is not None
        self.output_device = device
        self.gen_type = kind
        self.gen_level = 10**(level_db/20)
        self.gen_freq = float(freq)
        self.phase = self.sweep_phase = 0.0
        self.gen_on = True
        try:
            info = sd.query_devices(device, "output")
            output_channels = min(2, int(info["max_output_channels"]))
            if output_channels < 1:
                raise ValueError("Perangkat output tidak memiliki kanal speaker")
            if capture_active:
                self.stop_input()
                self._reset_capture()

                def duplex_callback(indata, outdata, frames, time, status):
                    y = self._generate(frames)
                    outdata[:] = y[:, None]
                    self._capture(indata, y)

                self.stream = sd.Stream(device=(self.input_device, device),
                                        channels=(self.channels, output_channels), samplerate=self.fs,
                                        callback=duplex_callback, blocksize=1024, dtype="float32")
                self.duplex = True
                self.stream.start()
            else:
                def output_callback(outdata, frames, time, status):
                    outdata[:] = self._generate(frames)[:, None]

                self.out_stream = sd.OutputStream(device=device, channels=output_channels,
                                                  samplerate=self.fs, callback=output_callback,
                                                  blocksize=1024, dtype="float32")
                self.out_stream.start()
        except Exception:
            self.stop_generator(resume_input=False)
            if capture_active:
                self.start_input(self.input_device, self.fs, self.channels)
            raise

    def stop_generator(self, resume_input=True):
        self.gen_on = False
        was_duplex = self.duplex
        if was_duplex:
            self.stop_input()
        if self.out_stream:
            stream, self.out_stream = self.out_stream, None
            try:
                stream.stop()
            finally:
                stream.close()
        if was_duplex and resume_input:
            self.start_input(self.input_device, self.fs, self.channels)

    def close(self):
        self.stop_generator(resume_input=False)
        self.stop_input()
