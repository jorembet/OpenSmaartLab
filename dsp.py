import numpy as np
from scipy import signal

EPS = 1e-20

def db20(x):
    return 20.0 * np.log10(np.maximum(np.abs(x), EPS))

def fft_spectrum(x, fs, nfft, window="hann"):
    x = np.asarray(x, dtype=np.float64)
    if len(x) < nfft:
        x = np.pad(x, (nfft-len(x), 0))
    else:
        x = x[-nfft:]
    w = signal.get_window(window, nfft, fftbins=True)
    scale = np.sum(w) / 2.0
    X = np.fft.rfft(x * w)
    f = np.fft.rfftfreq(nfft, 1.0/fs)
    mag = db20(np.abs(X) / max(scale, EPS))
    return f, mag, X

def fractional_octave_smooth(freq, values, fraction=12):
    if fraction <= 0 or len(freq) < 3:
        return values.copy()
    out = np.empty_like(values, dtype=float)
    ratio = 2 ** (1/(2*fraction))
    for i, f in enumerate(freq):
        if f <= 0:
            out[i] = values[i]
            continue
        lo, hi = f/ratio, f*ratio
        idx = (freq >= lo) & (freq <= hi)
        out[i] = np.mean(values[idx]) if np.any(idx) else values[i]
    return out

class TransferEstimator:
    def __init__(self, alpha=0.82):
        self.alpha = alpha
        self.Sxx = None
        self.Syy = None
        self.Sxy = None

    def reset(self):
        self.Sxx = self.Syy = self.Sxy = None

    def process(self, ref, meas, fs, nfft):
        ref = np.asarray(ref, float)
        meas = np.asarray(meas, float)
        if len(ref) < nfft:
            ref = np.pad(ref, (nfft-len(ref), 0))
        else:
            ref = ref[-nfft:]
        if len(meas) < nfft:
            meas = np.pad(meas, (nfft-len(meas), 0))
        else:
            meas = meas[-nfft:]

        w = signal.windows.hann(nfft, sym=False)
        X = np.fft.rfft(ref*w)
        Y = np.fft.rfft(meas*w)
        pxx = X*np.conj(X)
        pyy = Y*np.conj(Y)
        pxy = Y*np.conj(X)

        if self.Sxx is None or len(self.Sxx) != len(pxx):
            self.Sxx, self.Syy, self.Sxy = pxx, pyy, pxy
        else:
            a = self.alpha
            self.Sxx = a*self.Sxx + (1-a)*pxx
            self.Syy = a*self.Syy + (1-a)*pyy
            self.Sxy = a*self.Sxy + (1-a)*pxy

        H = self.Sxy / np.maximum(self.Sxx, EPS)
        coh = np.abs(self.Sxy)**2 / np.maximum(np.real(self.Sxx*self.Syy), EPS)
        coh = np.clip(np.real(coh), 0, 1)
        f = np.fft.rfftfreq(nfft, 1/fs)
        mag = db20(H)
        phase = np.unwrap(np.angle(H))*180/np.pi
        ir = np.fft.irfft(H, n=nfft)
        return f, mag, phase, coh, H, ir

def find_delay(ref, meas, fs, max_ms=500):
    ref = np.asarray(ref, float)
    meas = np.asarray(meas, float)
    n = min(len(ref), len(meas))
    if n < 64:
        return 0.0, 0
    ref = ref[-n:] - np.mean(ref[-n:])
    meas = meas[-n:] - np.mean(meas[-n:])
    corr = signal.correlate(meas, ref, mode="full", method="fft")
    lags = signal.correlation_lags(len(meas), len(ref), mode="full")
    maxlag = int(fs*max_ms/1000)
    mask = np.abs(lags) <= maxlag
    lag = int(lags[mask][np.argmax(np.abs(corr[mask]))])
    return lag/fs*1000.0, lag

def apply_delay(x, samples):
    if samples == 0:
        return x
    y = np.zeros_like(x)
    if samples > 0:
        y[samples:] = x[:-samples]
    else:
        s = -samples
        y[:-s] = x[s:]
    return y

def a_weighting_db(f):
    f = np.maximum(np.asarray(f, float), 1e-9)
    f2 = f*f
    ra = (12194**2 * f2**2) / (
        (f2 + 20.6**2)
        * np.sqrt((f2 + 107.7**2)*(f2 + 737.9**2))
        * (f2 + 12194**2)
    )
    return 20*np.log10(np.maximum(ra, EPS)) + 2.0

def c_weighting_db(f):
    f = np.maximum(np.asarray(f, float), 1e-9)
    f2 = f*f
    rc = (12194**2*f2) / ((f2+20.6**2)*(f2+12194**2))
    return 20*np.log10(np.maximum(rc, EPS)) + 0.06

def weighted_rms_dbfs(x, fs, weighting="Z"):
    x = np.asarray(x, float)
    n = max(1024, int(2**np.ceil(np.log2(max(32, len(x))))))
    X = np.fft.rfft(x, n=n)
    f = np.fft.rfftfreq(n, 1/fs)
    if weighting.upper() == "A":
        gain = 10**(a_weighting_db(f)/20)
    elif weighting.upper() == "C":
        gain = 10**(c_weighting_db(f)/20)
    else:
        gain = np.ones_like(f)
    y = np.fft.irfft(X*gain, n=n)[:len(x)]
    rms = np.sqrt(np.mean(y*y) + EPS)
    return 20*np.log10(rms + EPS)

def acoustics_from_ir(ir, fs):
    ir = np.asarray(ir, float)
    if not np.any(np.isfinite(ir)) or np.max(np.abs(ir)) < EPS:
        return {}
    e = ir*ir
    sch = np.cumsum(e[::-1])[::-1]
    sch /= max(sch[0], EPS)
    decay = 10*np.log10(np.maximum(sch, EPS))
    t = np.arange(len(ir))/fs

    def fit_rt(hi, lo):
        idx = np.where((decay <= hi) & (decay >= lo))[0]
        if len(idx) < 8:
            return np.nan
        p = np.polyfit(t[idx], decay[idx], 1)
        if p[0] >= 0:
            return np.nan
        return -60.0/p[0]

    edt = fit_rt(0, -10)
    rt20 = fit_rt(-5, -25)
    rt30 = fit_rt(-5, -35)
    rt60 = fit_rt(-5, -35)

    etc = db20(np.abs(ir)/max(np.max(np.abs(ir)), EPS))
    return {
        "time": t,
        "etc": etc,
        "decay": decay,
        "EDT": edt,
        "RT20": rt20,
        "RT30": rt30,
        "RT60": rt60,
    }

def pink_noise(n, state=None):
    # Frequency-domain 1/sqrt(f) noise, normalized
    rng = state if state is not None else np.random.default_rng()
    bins = n//2 + 1
    re = rng.normal(size=bins)
    im = rng.normal(size=bins)
    f = np.arange(bins, dtype=float)
    scale = np.ones_like(f)
    scale[1:] = 1/np.sqrt(f[1:])
    X = (re + 1j*im)*scale
    X[0] = 0
    y = np.fft.irfft(X, n=n)
    y /= max(np.max(np.abs(y)), EPS)
    return y
