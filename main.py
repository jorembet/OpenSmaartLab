import sys, csv, time, wave
from pathlib import Path
import numpy as np
from PySide6 import QtCore, QtWidgets, QtGui
import pyqtgraph as pg

from audio_engine import AudioEngine
from device_labels import device_label
from dsp import (
    fft_spectrum, fractional_octave_smooth, TransferEstimator,
    find_delay, apply_delay, acoustics_from_ir, weighted_rms_dbfs, db20
)

APP_NAME = "OpenSmaartLab"
DELAY_CHECK_INTERVAL = 0.75
DELAY_HYSTERESIS_SAMPLES = 4

pg.setConfigOption("background", "#111318")
pg.setConfigOption("foreground", "#d7dde8")
pg.setConfigOptions(antialias=True)


class MainWindow(QtWidgets.QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle(APP_NAME)
        self.resize(1400, 900)

        self.engine = AudioEngine()
        self.tf = TransferEstimator(alpha=.84)
        self.cal_offset = 120.0
        self.leq_energy = 0.0
        self.leq_samples = 0
        self.last_data = {}
        self.delay_samples = 0
        self.last_delay_check = 0.0
        self.spec_history = None
        self.spl_fast = 0.0
        self.spl_slow = 0.0
        self.peak_hold = None
        self.peak_hold_decay = 0.995

        self.build_ui()
        self.populate_devices()

        self.timer = QtCore.QTimer(self)
        self.timer.timeout.connect(self.update_measurements)
        self.timer.start(80)

    def build_ui(self):
        self.setStyleSheet("""
        QMainWindow,QWidget { background:#111318; color:#dde3ed; }
        QToolBar { background:#181b22; border-bottom:1px solid #2a2e38; spacing:8px; padding:6px; }
        QComboBox,QSpinBox,QDoubleSpinBox,QPushButton {
            background:#222631; border:1px solid #3b4150; border-radius:6px;
            padding:6px 10px; color:#ecf1f8; font-size:13px;
        }
        QComboBox:hover,QPushButton:hover { border-color:#5a6270; }
        QPushButton:checked { background:#8f263a; border-color:#a03040; }
        QCheckBox { spacing:6px; font-size:13px; padding:2px 4px; }
        QCheckBox::indicator { width:15px; height:15px; border:1px solid #3b4150;
            border-radius:4px; background:#222631; }
        QCheckBox::indicator:checked { background:#4fc3f7; border-color:#4fc3f7; }
        QTabWidget::pane { border:0; }
        QTabBar::tab { padding:10px 20px; background:#1a1e26; font-size:13px; }
        QTabBar::tab:selected { background:#2d3440; }
        QLabel#bigMeter { font-size:64px; font-weight:700; color:#4fc3f7; }
        QLabel#smallMeter { font-size:18px; color:#8892a0; }
        QGroupBox { border:1px solid #2a2e38; border-radius:8px; margin-top:8px; padding:12px; }
        QGroupBox::title { color:#8892a0; padding:0 8px; }
        """)

        tb = QtWidgets.QToolBar("Controls")
        tb.setMovable(False)
        self.addToolBar(tb)

        self.input_combo = QtWidgets.QComboBox()
        self.input_combo.setMinimumWidth(280)
        self.input_combo.setToolTip("Pilih perangkat input audio")

        self.output_combo = QtWidgets.QComboBox()
        self.output_combo.setMinimumWidth(280)
        self.output_combo.setToolTip("Pilih perangkat output audio untuk generator")

        self.start_btn = QtWidgets.QPushButton("Mulai")
        self.start_btn.setCheckable(True)
        self.start_btn.setMinimumWidth(100)
        self.start_btn.clicked.connect(self.toggle_input)
        self.start_btn.setToolTip("Mulai/Hentikan input audio (Spasi)")

        self.delay_btn = QtWidgets.QPushButton("Cari Delay")
        self.delay_btn.clicked.connect(self.request_delay)
        self.delay_btn.setToolTip("Cari delay antar channel sekarang (D)")

        self.auto_delay_cb = QtWidgets.QCheckBox("Auto")
        self.auto_delay_cb.setChecked(True)
        self.auto_delay_cb.setToolTip(
            "Cari delay otomatis untuk generator internal maupun referensi 2 kanal")
        self.auto_delay_cb.toggled.connect(self.on_auto_delay_toggled)

        self.capture_btn = QtWidgets.QPushButton("Simpan CSV")
        self.capture_btn.clicked.connect(self.capture_csv)
        self.capture_btn.setToolTip("Simpan data pengukuran ke CSV (C)")

        tb.addWidget(QtWidgets.QLabel("Input / Rekam:"))
        tb.addWidget(self.input_combo)
        tb.addWidget(QtWidgets.QLabel("Output / Putar:"))
        tb.addWidget(self.output_combo)
        tb.addSeparator()
        tb.addWidget(self.start_btn)
        tb.addWidget(self.delay_btn)
        tb.addWidget(self.auto_delay_cb)
        tb.addWidget(self.capture_btn)

        self.tabs = QtWidgets.QTabWidget()
        self.setCentralWidget(self.tabs)
        self.build_spl()
        self.build_realtime()
        self.build_ir()
        self.build_generator()
        self.tabs.setCurrentIndex(1)
        self.setup_shortcuts()

        self.status = self.statusBar()
        self.status.showMessage("Pilih perangkat input, lalu tekan 'Mulai'")

    def mkplot(self, title, xlabel="Frekuensi (Hz)", ylabel="dB"):
        p = pg.PlotWidget(title=title)
        p.showGrid(x=True, y=True, alpha=.22)
        p.setLabel("bottom", xlabel)
        p.setLabel("left", ylabel)
        return p

    def build_spl(self):
        w = QtWidgets.QWidget()
        outer = QtWidgets.QVBoxLayout(w)

        meter_box = QtWidgets.QGroupBox("Pengukur SPL")
        meter_layout = QtWidgets.QVBoxLayout(meter_box)

        self.spl_label = QtWidgets.QLabel("--.- dB")
        self.spl_label.setObjectName("bigMeter")
        self.spl_label.setAlignment(QtCore.Qt.AlignmentFlag.AlignCenter)
        meter_layout.addWidget(self.spl_label)

        sub_meters = QtWidgets.QHBoxLayout()
        self.peak_label = QtWidgets.QLabel("Peak: --.-")
        self.peak_label.setObjectName("smallMeter")
        self.peak_label.setAlignment(QtCore.Qt.AlignmentFlag.AlignCenter)
        self.leq_label = QtWidgets.QLabel("Leq: --.-")
        self.leq_label.setObjectName("smallMeter")
        self.leq_label.setAlignment(QtCore.Qt.AlignmentFlag.AlignCenter)
        sub_meters.addWidget(self.peak_label)
        sub_meters.addWidget(self.leq_label)
        meter_layout.addLayout(sub_meters)

        outer.addWidget(meter_box)

        settings_box = QtWidgets.QGroupBox("Pengaturan")
        settings_layout = QtWidgets.QHBoxLayout(settings_box)

        self.weight_combo = QtWidgets.QComboBox()
        self.weight_combo.addItems(["A", "C", "Z"])
        self.weight_combo.setToolTip("Frekuensi weighting: A untuk suara manusia, C untuk musik, Z flat")
        settings_layout.addWidget(QtWidgets.QLabel("Weighting:"))
        settings_layout.addWidget(self.weight_combo)

        self.response_combo = QtWidgets.QComboBox()
        self.response_combo.addItems(["Fast", "Slow"])
        self.response_combo.setToolTip("Fast = respons cepat (125ms), Slow = respons lambat (1 detik)")
        settings_layout.addWidget(QtWidgets.QLabel("Respons:"))
        settings_layout.addWidget(self.response_combo)

        self.cal_spin = QtWidgets.QDoubleSpinBox()
        self.cal_spin.setRange(-50, 200)
        self.cal_spin.setDecimals(1)
        self.cal_spin.setValue(self.cal_offset)
        self.cal_spin.setSuffix(" dB")
        self.cal_spin.setToolTip("Offset kalibrasi - gunakan kalibrator 94dB@1kHz")
        self.cal_spin.valueChanged.connect(lambda v: setattr(self, "cal_offset", v))
        settings_layout.addWidget(QtWidgets.QLabel("Kalibrasi:"))
        settings_layout.addWidget(self.cal_spin)

        reset_btn = QtWidgets.QPushButton("Reset Leq")
        reset_btn.clicked.connect(self.reset_leq)
        reset_btn.setToolTip("Reset rata-rata Leq (R)")
        settings_layout.addWidget(reset_btn)

        outer.addWidget(settings_box)

        self.spl_plot = self.mkplot("Riwayat SPL", "Waktu (detik)", "dB SPL")
        self.spl_curve = self.spl_plot.plot(pen=pg.mkPen(width=2, color="#4fc3f7"))
        self.spl_hist_t = []
        self.spl_hist_v = []
        outer.addWidget(self.spl_plot)

        self.tabs.addTab(w, "SPL Meter")

    def build_realtime(self):
        w = QtWidgets.QWidget()
        layout = QtWidgets.QGridLayout(w)

        self.rta_plot = self.mkplot("RTA Microphone — Frekuensi / dBFS")
        self.rta_plot.setLogMode(x=True, y=False)
        self.rta_plot.setYRange(-120, 0)
        self.rta_plot.setXRange(np.log10(20), np.log10(20000), padding=0)
        bands = [20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500,
                 630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300,
                 8000, 10000, 12500, 16000, 20000]
        self.rta_bands = np.asarray(bands)
        self.rta_plot.getAxis("bottom").setTicks([[(np.log10(hz), f"{hz/1000:g}k" if hz >= 1000 else f"{hz:g}") for hz in bands]])
        self.rta_plot.addLegend()
        self.generator_curve = self.rta_plot.plot(pen=pg.mkPen(width=1, color="#ffb74d"), name="Output generator")
        self.rta_curve = self.rta_plot.plot(pen=pg.mkPen(width=2, color="#4fc3f7"), name="Microphone")
        self.mic_bars = pg.BarGraphItem(x=np.log10(self.rta_bands), height=np.zeros(31), width=.025, y0=-120, brush="#4fc3f7", pen=None)
        self.output_bars = pg.BarGraphItem(x=np.log10(self.rta_bands)+.03, height=np.zeros(31), width=.025, y0=-120, brush="#ffb74d", pen=None)
        self.rta_plot.addItem(self.mic_bars)
        self.rta_plot.addItem(self.output_bars)
        self.rta_peak_curve = self.rta_plot.plot(pen=pg.mkPen(width=1, color=(255, 100, 100, 180)))
        self.rta_plot.scene().sigMouseMoved.connect(self.on_rta_mouse_moved)

        self.spectro_plot = pg.PlotWidget(title="Spektrogram (Waktu vs Frekuensi)")
        self.spectro_img = pg.ImageItem()
        cmap = pg.ColorMap([0, 0.25, 0.5, 0.75, 1.0], [
            (0, 0, 0), (80, 0, 120), (200, 50, 0), (255, 200, 0), (255, 255, 255)
        ])
        self.spectro_img.setLookupTable(cmap.getLookupTable())
        self.spectro_plot.addItem(self.spectro_img)
        self.spectro_plot.setLabel("bottom", "Frekuensi (Hz)")
        self.spectro_plot.setLabel("left", "Waktu")

        self.tf_plot = self.mkplot("Transfer Function - Magnitudo")
        self.tf_plot.setLogMode(x=True, y=False)
        self.tf_curve = self.tf_plot.plot(pen=pg.mkPen(width=2, color="#81c784"))
        self.tf_plot.setYRange(-36, 24)

        self.phase_plot = self.mkplot("Transfer Function - Fase", ylabel="Derajat")
        self.phase_plot.setLogMode(x=True, y=False)
        self.phase_curve = self.phase_plot.plot(pen=pg.mkPen(width=1.5, color="#ffb74d"))
        self.phase_plot.setYRange(-180, 180)

        self.coh_plot = self.mkplot("Koherensi", ylabel="0-1")
        self.coh_plot.setLogMode(x=True, y=False)
        self.coh_curve = self.coh_plot.plot(pen=pg.mkPen(width=2, color="#ba68c8"))
        self.coh_plot.setYRange(0, 1.05)

        controls = QtWidgets.QHBoxLayout()
        self.mic_channel = QtWidgets.QComboBox()
        self.mic_channel.addItems(["Mic Ch1 / Ref Ch2", "Mic Ch2 / Ref Ch1"])
        self.mic_channel.setToolTip("Microphone mono selalu memakai Ch1. Delay perlu referensi sinyal yang sama pada kanal lain.")
        self.mic_channel.currentIndexChanged.connect(self.reset_routing)
        controls.addWidget(self.mic_channel)
        self.rta_style = QtWidgets.QComboBox()
        self.rta_style.addItems(["Bar 31 band", "Line"])
        self.rta_style.setToolTip("Bar: puncak FFT per band 1/3 oktaf. Line: spektrum FFT.")
        controls.addWidget(self.rta_style)
        self.smooth_combo = QtWidgets.QComboBox()
        self.smooth_combo.addItems(["None", "1/3", "1/6", "1/12", "1/24", "1/48"])
        self.smooth_combo.setCurrentText("1/12")
        self.smooth_combo.setToolTip("Smoothing oktaf - semakin halus semakin rata")
        controls.addWidget(QtWidgets.QLabel("Smoothing:"))
        controls.addWidget(self.smooth_combo)

        self.avg_combo = QtWidgets.QComboBox()
        self.avg_combo.addItems(["Fast", "Medium", "Slow"])
        self.avg_combo.setToolTip("Averaging transfer function")
        controls.addWidget(QtWidgets.QLabel("Averaging:"))
        controls.addWidget(self.avg_combo)

        self.delay_label = QtWidgets.QLabel("Delay: -- ms")
        self.cursor_label = QtWidgets.QLabel("Kursor: -- Hz, -- dB")
        controls.addStretch()
        controls.addWidget(self.delay_label)
        controls.addWidget(self.cursor_label)

        generator_controls = QtWidgets.QHBoxLayout()
        self.pink_btn = QtWidgets.QPushButton("Play Pink Noise")
        self.pink_btn.clicked.connect(self.play_pink_noise)
        self.pink_level = QtWidgets.QDoubleSpinBox()
        self.pink_level.setRange(-90, -3)
        self.pink_level.setValue(-24)
        self.pink_level.setSuffix(" dBFS")
        self.pink_level.valueChanged.connect(self.set_pink_level)
        generator_controls.addWidget(self.pink_btn)
        generator_controls.addWidget(self.pink_level)
        generator_controls.addWidget(QtWidgets.QLabel("Output → speaker → microphone | Delay total termasuk latensi audio"))
        header = QtWidgets.QVBoxLayout()
        header.addLayout(generator_controls)
        header.addLayout(controls)
        layout.addLayout(header, 0, 0, 1, 2)
        layout.addWidget(self.rta_plot, 1, 0)
        layout.addWidget(self.spectro_plot, 1, 1)
        layout.addWidget(self.tf_plot, 2, 0)
        layout.addWidget(self.phase_plot, 2, 1)
        layout.addWidget(self.coh_plot, 3, 0, 1, 2)
        self.tabs.addTab(w, "Analisis Real-Time")

    def build_ir(self):
        w = QtWidgets.QWidget()
        lay = QtWidgets.QGridLayout(w)

        self.ir_plot = self.mkplot("Impulse Response", "Waktu (ms)", "Amplitudo")
        self.ir_curve = self.ir_plot.plot(pen=pg.mkPen(width=1.5, color="#4fc3f7"))

        self.etc_plot = self.mkplot("Energy Time Curve", "Waktu (ms)", "dB")
        self.etc_curve = self.etc_plot.plot(pen=pg.mkPen(width=1.5, color="#81c784"))

        self.decay_plot = self.mkplot("Kurva Peluruhan (Schroeder)", "Waktu (detik)", "dB")
        self.decay_curve = self.decay_plot.plot(pen=pg.mkPen(width=2, color="#ffb74d"))

        self.rt_label = QtWidgets.QLabel("EDT: --   RT20: --   RT30: --   RT60: --")
        self.rt_label.setStyleSheet("font-size:16px;padding:10px;color:#4fc3f7;")
        self.rt_label.setAlignment(QtCore.Qt.AlignmentFlag.AlignCenter)

        self.ir_export_btn = QtWidgets.QPushButton("Export ke WAV")
        self.ir_export_btn.clicked.connect(self.export_ir_wav)
        self.ir_export_btn.setToolTip("Simpan impulse response sebagai file WAV")

        header = QtWidgets.QHBoxLayout()
        header.addWidget(self.rt_label)
        header.addWidget(self.ir_export_btn)

        lay.addLayout(header, 0, 0, 1, 2)
        lay.addWidget(self.ir_plot, 1, 0)
        lay.addWidget(self.etc_plot, 1, 1)
        lay.addWidget(self.decay_plot, 2, 0, 1, 2)
        self.tabs.addTab(w, "Impulse Response")

    def build_generator(self):
        w = QtWidgets.QWidget()
        lay = QtWidgets.QVBoxLayout(w)

        box = QtWidgets.QGroupBox("Generator Sinyal")
        form = QtWidgets.QFormLayout(box)

        self.gen_type = QtWidgets.QComboBox()
        self.gen_type.addItems(["Pink Noise", "White Noise", "Sine", "Sweep"])
        self.gen_type.setToolTip("Jenis sinyal: Pink untuk pengukuran ruangan, Sine untuk frekuensi spesifik")
        form.addRow("Jenis Sinyal:", self.gen_type)

        self.gen_db = QtWidgets.QDoubleSpinBox()
        self.gen_db.setRange(-90, -3)
        self.gen_db.setValue(-24)
        self.gen_db.setSuffix(" dBFS")
        self.gen_db.setToolTip("Level output sinyal")
        form.addRow("Level:", self.gen_db)

        self.gen_freq = QtWidgets.QDoubleSpinBox()
        self.gen_freq.setRange(10, 24000)
        self.gen_freq.setValue(1000)
        self.gen_freq.setSuffix(" Hz")
        self.gen_freq.setToolTip("Frekuensi untuk sinyal Sine")
        form.addRow("Frekuensi Sine:", self.gen_freq)

        self.gen_btn = QtWidgets.QPushButton("Nyalakan Generator")
        self.gen_btn.setCheckable(True)
        self.gen_btn.setMinimumHeight(50)
        self.gen_btn.clicked.connect(self.toggle_generator)
        self.gen_btn.setToolTip("Nyalakan/Matikan generator sinyal (G)")
        form.addRow(self.gen_btn)

        lay.addWidget(box)
        lay.addStretch()
        self.tabs.addTab(w, "Generator")

    def setup_shortcuts(self):
        QtGui.QShortcut(QtGui.QKeySequence("Space"), self, self.start_btn.click)
        QtGui.QShortcut(QtGui.QKeySequence("D"), self, self.request_delay)
        QtGui.QShortcut(QtGui.QKeySequence("C"), self, self.capture_btn.click)
        QtGui.QShortcut(QtGui.QKeySequence("R"), self, self.reset_leq)
        QtGui.QShortcut(QtGui.QKeySequence("G"), self, self.gen_btn.click)
        QtGui.QShortcut(QtGui.QKeySequence("1"), self, lambda: self.tabs.setCurrentIndex(0))
        QtGui.QShortcut(QtGui.QKeySequence("2"), self, lambda: self.tabs.setCurrentIndex(1))
        QtGui.QShortcut(QtGui.QKeySequence("3"), self, lambda: self.tabs.setCurrentIndex(2))
        QtGui.QShortcut(QtGui.QKeySequence("4"), self, lambda: self.tabs.setCurrentIndex(3))
        QtGui.QShortcut(QtGui.QKeySequence("P"), self, self.toggle_peak_hold)

    def on_rta_mouse_moved(self, pos):
        if not self.last_data or "freq" not in self.last_data:
            return
        mouse_point = self.rta_plot.vb.mapSceneToView(pos)
        x = mouse_point.x()
        freq = 10 ** x if x > 0 else 0
        f = self.last_data["freq"]
        spec = self.last_data["spectrum"]
        if len(f) > 0 and f[0] <= freq <= f[-1]:
            idx = np.argmin(np.abs(f - freq))
            self.cursor_label.setText(f"Kursor: {f[idx]:.1f} Hz, {spec[idx]:.1f} dB")
        else:
            self.cursor_label.setText("Kursor: -- Hz, -- dB")

    def toggle_peak_hold(self):
        if self.peak_hold is None:
            self.peak_hold = np.array([])
            self.status.showMessage("Peak hold aktif")
        else:
            self.peak_hold = None
            self.rta_peak_curve.setData([], [])
            self.status.showMessage("Peak hold nonaktif")

    def export_ir_wav(self):
        if not self.last_data or "ir" not in self.last_data:
            self.status.showMessage("Belum ada impulse response")
            return
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "Export IR", f"ir-{int(time.time())}.wav", "WAV (*.wav)"
        )
        if not path:
            return
        ir = self.last_data["ir"]
        ir = ir / max(np.max(np.abs(ir)), 1e-20)
        ir_int16 = (ir * 32767).astype(np.int16)
        with wave.open(path, "w") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(self.engine.fs)
            wf.writeframes(ir_int16.tobytes())
        self.status.showMessage(f"IR tersimpan: {path}")

    def populate_devices(self):
        self.input_combo.clear()
        self.output_combo.clear()
        try:
            for i, d in enumerate(self.engine.devices()):
                if d["max_input_channels"] > 0:
                    self.input_combo.addItem(device_label(d["name"], True, d["max_input_channels"]), i)
                    self.input_combo.setItemData(self.input_combo.count() - 1, self.input_combo.itemText(self.input_combo.count() - 1), QtCore.Qt.ToolTipRole)
                if d["max_output_channels"] > 0:
                    self.output_combo.addItem(device_label(d["name"], False, d["max_output_channels"]), i)
                    self.output_combo.setItemData(self.output_combo.count() - 1, self.output_combo.itemText(self.output_combo.count() - 1), QtCore.Qt.ToolTipRole)
        except Exception as e:
            self.status.showMessage(str(e))

    def toggle_input(self, on):
        if on:
            try:
                dev = self.input_combo.currentData()
                info = self.engine.devices()[dev]
                channels = min(2, int(info["max_input_channels"]))
                if channels < 1:
                    raise ValueError("Pilih perangkat microphone dengan kanal input")
                self.engine.start_input(dev, 48000, channels)
                self.reset_routing()
                self.tabs.setCurrentIndex(1)
                self.start_btn.setText("Berhenti")
                self.status.showMessage(f"Aktif: {info['name']}")
            except Exception as e:
                self.start_btn.setChecked(False)
                self.start_btn.setText("Mulai")
                QtWidgets.QMessageBox.critical(self, "Error Audio", str(e))
        else:
            self.engine.stop_generator(resume_input=False)
            self.sync_generator_controls()
            self.engine.stop_input()
            self.start_btn.setText("Mulai")

    def set_pink_level(self, value):
        self.gen_db.setValue(value)
        self.engine.gen_level = 10**(value/20)

    def sync_generator_controls(self):
        on = self.engine.gen_on
        self.gen_btn.setChecked(on)
        self.gen_btn.setText("Matikan Generator" if on else "Nyalakan Generator")
        self.pink_btn.setText("Stop Generator" if on else "Play Pink Noise")
        if not on:
            self.generator_curve.setData([], [])
        self.delay_label.setText("Delay: -- ms")

    def play_pink_noise(self):
        if self.engine.gen_on:
            self.toggle_generator(False)
            return
        if self.engine.stream is None:
            self.start_btn.setChecked(True)
            self.toggle_input(True)
        if self.engine.stream is not None:
            self.gen_type.setCurrentText("Pink Noise")
            self.toggle_generator(True)
            self.tabs.setCurrentIndex(1)

    def toggle_generator(self, on):
        try:
            if on:
                self.engine.start_generator(self.output_combo.currentData(), self.gen_type.currentText(),
                                            self.gen_db.value(), self.gen_freq.value())
            else:
                self.engine.stop_generator()
            self.reset_routing()
        except Exception as e:
            QtWidgets.QMessageBox.critical(self, "Error Generator", str(e))
        self.sync_generator_controls()

    def smoothing_fraction(self):
        s = self.smooth_combo.currentText()
        return 0 if s == "None" else int(s.split("/")[1])

    def update_measurements(self):
        nfft = 16384
        data, generated = self.engine.get_latest_pair(nfft)
        if len(data) < nfft or data.shape[1] < 1:
            return

        mic_index = self.mic_channel.currentIndex() if data.shape[1] > 1 else 0
        meas = data[:, mic_index].astype(float)
        ref = data[:, 1 - mic_index].astype(float) if data.shape[1] > 1 else np.zeros_like(meas)
        internal_reference = self.engine.gen_on and self.engine.duplex
        if internal_reference:
            ref = generated
        has_reference = (internal_reference or data.shape[1] > 1) and np.mean(ref ** 2) > 1e-8 and np.mean(meas ** 2) > 1e-8
        fs = self.engine.fs

        f, spec, _ = fft_spectrum(meas, fs, nfft)
        raw_spec = spec.copy()
        frac = self.smoothing_fraction()
        if frac:
            spec = fractional_octave_smooth(f, spec, frac)
        mask = (f >= 20) & (f <= min(24000, fs / 2))
        self.rta_curve.setData(f[mask], spec[mask])
        if internal_reference:
            _, output_spec, _ = fft_spectrum(ref, fs, nfft)
            raw_output = output_spec.copy()
            if frac:
                output_spec = fractional_octave_smooth(f, output_spec, frac)
            self.generator_curve.setData(f[mask], output_spec[mask])
        else:
            self.generator_curve.setData([], [])

        self.auto_delay_check()

        bars = self.rta_style.currentIndex() == 0
        self.mic_bars.setVisible(bars)
        self.output_bars.setVisible(bars and internal_reference)
        self.rta_curve.setVisible(not bars)
        self.generator_curve.setVisible(not bars and internal_reference)
        if bars:
            def band_peaks(values):
                ratio = 2**(1/6)
                return np.array([max(values[(f >= hz/ratio) & (f < hz*ratio)], default=-120) for hz in self.rta_bands])
            self.mic_bars.setOpts(height=np.clip(band_peaks(raw_spec), -120, 0)+120)
            if internal_reference:
                self.output_bars.setOpts(height=np.clip(band_peaks(raw_output), -120, 0)+120)

        if self.peak_hold is not None:
            if len(self.peak_hold) != len(spec):
                self.peak_hold = spec.copy()
            else:
                self.peak_hold = np.maximum(self.peak_hold * self.peak_hold_decay, spec)
            self.rta_peak_curve.setData(f[mask], self.peak_hold[mask])

        row = spec[mask]
        if self.spec_history is None or self.spec_history.shape[1] != len(row):
            self.spec_history = np.tile(row, (120, 1))
        else:
            self.spec_history = np.roll(self.spec_history, -1, axis=0)
            self.spec_history[-1] = row
        self.spectro_img.setImage(self.spec_history, autoLevels=False, levels=(-100, 0))
        self.spectro_img.setRect(QtCore.QRectF(f[mask][0], 0, f[mask][-1] - f[mask][0], 120))

        if has_reference:
            favg = {"Fast": .55, "Medium": .82, "Slow": .94}[self.avg_combo.currentText()]
            self.tf.alpha = favg
            ref_tf = apply_delay(ref, self.delay_samples) if self.delay_samples else ref
            f2, mag, phase, coh, H, ir = self.tf.process(ref_tf, meas, fs, nfft)
            if frac:
                mag = fractional_octave_smooth(f2, mag, frac)
                phase = fractional_octave_smooth(f2, phase, frac)
            m2 = (f2 >= 20) & (f2 <= min(24000, fs / 2))
            ph = (phase + 180) % 360 - 180
            self.tf_curve.setData(f2[m2], mag[m2])
            self.phase_curve.setData(f2[m2], ph[m2])
            self.coh_curve.setData(f2[m2], coh[m2])

            ir_shift = np.roll(ir, nfft // 2)
            tms = (np.arange(nfft) - nfft // 2) / fs * 1000
            irn = ir_shift / max(np.max(np.abs(ir_shift)), 1e-20)
            self.ir_curve.setData(tms, irn)
            ac = acoustics_from_ir(np.roll(ir, 0), fs)
            if ac:
                self.etc_curve.setData(np.arange(len(ir)) / fs * 1000, ac["etc"])
                self.decay_curve.setData(ac["time"], ac["decay"])
                def fmt(v): return "--" if not np.isfinite(v) else f"{v:.2f}s"
                self.rt_label.setText(
                    f"EDT: {fmt(ac['EDT'])}   RT20: {fmt(ac['RT20'])}   "
                    f"RT30: {fmt(ac['RT30'])}   RT60: {fmt(ac['RT60'])}"
                )

        else:
            self.delay_label.setText("Delay: -- (perlu referensi)")
            for curve in (self.tf_curve, self.phase_curve, self.coh_curve, self.ir_curve, self.etc_curve, self.decay_curve):
                curve.setData([], [])
            self.rt_label.setText("Reverberation: perlu referensi")
            f2, m2 = f, mask
            mag = ph = coh = np.full_like(f, np.nan)
            ir = np.array([])

        weighting = self.weight_combo.currentText()
        dbfs = weighted_rms_dbfs(meas, fs, weighting)
        spl_instant = dbfs + self.cal_offset
        response = self.response_combo.currentText()
        tau = 0.125 if response == "Fast" else 1.0
        alpha = 1.0 - np.exp(-1.0 / (tau * fs))
        if self.spl_fast == 0.0:
            self.spl_fast = spl_instant
            self.spl_slow = spl_instant
        else:
            self.spl_fast += alpha * (spl_instant - self.spl_fast)
            self.spl_slow += alpha * (spl_instant - self.spl_slow)
        spl = self.spl_fast if response == "Fast" else self.spl_slow
        peak = db20(np.max(np.abs(meas))) + self.cal_offset
        self.leq_energy += np.sum(meas * meas)
        self.leq_samples += len(meas)
        leq = 10 * np.log10(max(self.leq_energy / max(self.leq_samples, 1), 1e-20)) + self.cal_offset
        self.spl_label.setText(f"{spl:.1f} dB{weighting}")
        self.peak_label.setText(f"Peak: {peak:.1f}")
        self.leq_label.setText(f"Leq: {leq:.1f}")

        now = time.monotonic()
        self.spl_hist_t.append(now)
        self.spl_hist_v.append(spl)
        if len(self.spl_hist_t) > 900:
            self.spl_hist_t = self.spl_hist_t[-900:]
            self.spl_hist_v = self.spl_hist_v[-900:]
        t0 = self.spl_hist_t[0]
        self.spl_curve.setData(np.array(self.spl_hist_t) - t0, self.spl_hist_v)

        self.last_data = {
            "freq": f2[m2], "mag": mag[m2], "phase": ph[m2],
            "coh": coh[m2], "spectrum": spec[mask], "ir": ir
        }

    def reset_routing(self, *_):
        self.tf = TransferEstimator(alpha=.84)
        self.delay_samples = 0
        self.last_delay_check = 0.0
        self.delay_label.setText("Delay: -- ms")
        self.spec_history = None
        self.peak_hold = None
        self.rta_peak_curve.setData([], [])
        self.last_data = {}

    def request_delay(self):
        self.do_find_delay()

    def on_auto_delay_toggled(self, on):
        self.last_delay_check = 0.0
        if on:
            self.auto_delay_check()

    def auto_delay_check(self):
        if not self.auto_delay_cb.isChecked():
            return
        now = time.monotonic()
        if now - self.last_delay_check < DELAY_CHECK_INTERVAL:
            return
        self.last_delay_check = now
        self.do_find_delay(quiet=True)

    def delay_feedback(self, text, message=None, quiet=False):
        if quiet:
            return
        self.delay_label.setText(text)
        if message:
            self.status.showMessage(message)

    def do_find_delay(self, quiet=False):
        internal = self.engine.gen_on and self.engine.duplex
        n = 65536 if internal else 16384
        data, generated = self.engine.get_latest_pair(n)
        if len(data) < n or (not internal and data.shape[1] < 2):
            self.delay_feedback("Delay: -- (menunggu sinyal / referensi)", quiet=quiet)
            return
        if internal and self.engine.gen_type not in ("Pink", "Pink Noise", "White", "White Noise"):
            self.delay_feedback("Delay: -- (gunakan pink noise)", quiet=quiet)
            return
        mic_index = self.mic_channel.currentIndex() if data.shape[1] > 1 else 0
        ref = generated if internal else data[:, 1 - mic_index]
        meas = data[:, mic_index]
        if np.mean(ref ** 2) <= 1e-8 or np.mean(meas ** 2) <= 1e-8:
            self.delay_feedback(
                "Delay: -- (sinyal terlalu kecil)",
                "Hubungkan referensi dan microphone; keduanya harus menerima sinyal", quiet=quiet)
            return
        ms, samples = find_delay(ref, meas, self.engine.fs)
        aligned_ref = ref[:len(ref)-samples] if samples >= 0 else ref[-samples:]
        aligned_meas = meas[samples:] if samples >= 0 else meas[:len(meas)+samples]
        confidence = abs(np.corrcoef(aligned_ref, aligned_meas)[0, 1])
        if not np.isfinite(confidence) or confidence < (0.2 if internal else 0.5) or (internal and not 0 <= ms < 499):
            self.delay_feedback(
                "Delay: -- (referensi tidak cocok)",
                "Gunakan sinyal broadband yang sama pada referensi dan microphone", quiet=quiet)
            return
        if abs(samples - self.delay_samples) > DELAY_HYSTERESIS_SAMPLES:
            self.delay_samples = samples
            self.tf.reset()
        else:
            samples = self.delay_samples
            ms = samples/self.engine.fs*1000.0
        text = f"{'Delay total' if internal else 'Delay'}: {ms:.3f} ms"
        self.delay_label.setText(f"{text} (auto)" if quiet else text)
        if not quiet:
            self.status.showMessage(f"Delay terukur: {ms:.3f} ms ({samples} samples)")

    def reset_leq(self):
        self.leq_energy = 0
        self.leq_samples = 0

    def capture_csv(self):
        if not self.last_data:
            self.status.showMessage("Belum ada data untuk disimpan")
            return
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "Simpan Data", f"trace-{int(time.time())}.csv", "CSV (*.csv)"
        )
        if not path:
            return
        d = self.last_data
        n = min(map(len, [d["freq"], d["mag"], d["phase"], d["coh"], d["spectrum"]]))
        with open(path, "w", newline="") as f:
            wr = csv.writer(f)
            wr.writerow(["frekuensi_hz", "tf_magnitudo_db", "fase_derajat", "koherensi", "spektrum_dbfs"])
            for i in range(n):
                wr.writerow([d["freq"][i], d["mag"][i], d["phase"][i], d["coh"][i], d["spectrum"][i]])
        self.status.showMessage(f"Data tersimpan: {path}")

    def closeEvent(self, event):
        self.engine.close()
        event.accept()


if __name__ == "__main__":
    app = QtWidgets.QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    font = QtGui.QFont("DejaVu Sans", 11)
    app.setFont(font)
    win = MainWindow()
    win.show()
    sys.exit(app.exec())
