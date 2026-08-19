"""
dualmic — offline dual-microphone voice-beamforming prototypes for the Halo.

This is a *library* (import it; the CLIs are beamform.py / synth.py). It contains
the DSP for boosting broadside voice and attenuating off-axis / diffuse noise on
the Halo's two edge-multiplexed T5838 PDM mics, exploiting the geometry the
product gives us for free:

  - The wearer sits on the perpendicular bisector of the two mics -> their voice
    (and anyone they face) arrives at BOTH mics at the same time (zero ITD) and
    equal level (zero ILD): in-phase at every frequency.
  - Anything off to the side arrives with a nonzero inter-mic delay tau = d*sin(theta)/c.

Two trivial linear combinations fall out and anchor everything below:
  SUM  = L+R  ->  max coherent gain at broadside; frequency/angle-dependent notch off-axis
  DIFF = L-R  ->  EXACT zero at broadside -> contains no target, only off-axis/diffuse
                  energy. A perfect, steering-error-free noise reference / blocker.

Methods implemented (see README.md for the physics and the honest limitations):
  A. gsc_lite   - SUM cleaned by an NLMS adaptive canceller driven by the DIFF
                  reference. Adapts a null onto a dominant *lateral* interferer.
  B. ipd_mask   - STFT soft mask: pass a T-F bin only if it looks broadside
                  (IPD~=0) AND level-matched (ILD~=0) AND in the voice band.
  C. cdr_mask   - STFT soft mask from the coherent-to-diffuse ratio: pass coherent
                  (directional) energy, suppress diffuse (babble/wind/reverb) using
                  the analytic diffuse-field coherence sinc(2 f d / c).
  B and C multiply into one combined mask (recommended); either can post-filter A.

IMPORTANT geometric limitation (stated up front so nobody expects magic): two mics
on a left-right axis can only resolve the left-right axis. They CANNOT tell "in
front" from "behind" or "above" - all of those are broadside (zero ITD) and are
treated identically. So this rejects sources to the *sides* well and cannot reject
a source directly behind the wearer. For "talker in front + interferers to the
side", that is exactly the wanted behaviour.

Correction to an earlier design note: a diffuse-noise *superdirective* (MVDR)
beamformer collapses to plain delay-sum for a broadside target on a symmetric
pair (the steering vector [1,1] is an eigenvector of the diffuse coherence
matrix), so it buys nothing here. The real third lever is the coherent/diffuse
ratio (method C), not superdirectivity.
"""

from __future__ import annotations

import numpy as np
from scipy import signal
from scipy.io import wavfile

C_SOUND = 343.0  # m/s

# ---------------------------------------------------------------------------
# I/O
# ---------------------------------------------------------------------------


def read_stereo(path):
    """Load a WAV. Returns (L, R, fs) as float32 in [-1, 1].

    Accepts stereo (uses ch0=L, ch1=R) or mono (duplicates to both channels).
    """
    fs, data = wavfile.read(path)
    data = _to_float(data)
    if data.ndim == 1:
        return data.copy(), data.copy(), fs
    return np.ascontiguousarray(data[:, 0]), np.ascontiguousarray(data[:, 1]), fs


def read_mono(path):
    """Load a WAV as a single float32 channel (averages if stereo)."""
    fs, data = wavfile.read(path)
    data = _to_float(data)
    if data.ndim > 1:
        data = data.mean(axis=1)
    return data, fs


def write_mono(path, x, fs):
    """Write a float [-1,1] mono signal as 16-bit PCM WAV (clips safely)."""
    y = np.clip(x, -1.0, 1.0)
    wavfile.write(path, int(fs), (y * 32767.0).astype(np.int16))


def write_stereo(path, left, right, fs):
    n = min(len(left), len(right))
    y = np.stack([np.clip(left[:n], -1, 1), np.clip(right[:n], -1, 1)], axis=1)
    wavfile.write(path, int(fs), (y * 32767.0).astype(np.int16))


def _to_float(data):
    if data.dtype == np.int16:
        return data.astype(np.float32) / 32768.0
    if data.dtype == np.int32:
        return data.astype(np.float32) / 2147483648.0
    if data.dtype == np.uint8:  # WAV 8-bit is unsigned
        return (data.astype(np.float32) - 128.0) / 128.0
    if np.issubdtype(data.dtype, np.floating):
        return data.astype(np.float32)
    raise ValueError(f"unsupported WAV dtype {data.dtype}")


# ---------------------------------------------------------------------------
# Fractional delay / gain (used by calibration and the synth generator)
# ---------------------------------------------------------------------------


def frac_delay(x, delay_samples):
    """Delay x by a (possibly fractional / negative) number of samples via an
    FFT phase ramp. Positive delay shifts the signal later in time."""
    n = len(x)
    X = np.fft.rfft(x)
    k = np.fft.rfftfreq(n) * n  # bin index 0..n/2
    X = X * np.exp(-2j * np.pi * k * delay_samples / n)
    return np.fft.irfft(X, n).astype(np.float32)


# ---------------------------------------------------------------------------
# Calibration: measure inter-mic delay (=> spacing d) and gain/phase mismatch
# ---------------------------------------------------------------------------


def gcc_phat(x, y, fs, max_tau=None, interp=16):
    """Generalized cross-correlation with phase transform.

    Returns (tau_seconds, cc, lags_seconds). tau > 0 means y is delayed relative
    to x (x arrives first). PHAT weighting makes the peak sharp and robust to the
    source spectrum - the standard robust TDOA estimator.
    """
    n = len(x) + len(y)
    X = np.fft.rfft(x, n)
    Y = np.fft.rfft(y, n)
    R = X * np.conj(Y)
    R /= np.abs(R) + 1e-12
    cc = np.fft.irfft(R, n * interp)
    max_shift = int(interp * n / 2)
    if max_tau is not None:
        max_shift = min(int(interp * fs * max_tau), max_shift)
    cc = np.concatenate((cc[-max_shift:], cc[: max_shift + 1]))
    shift = np.argmax(np.abs(cc)) - max_shift
    tau = shift / float(interp * fs)
    lags = (np.arange(len(cc)) - max_shift) / float(interp * fs)
    return tau, cc, lags


def estimate_geometry(left, right, fs, frame_ms=32):
    """Estimate array geometry + mic match from a capture.

    Returns a dict with:
      delay_samples   global GCC-PHAT inter-mic delay (L relative to R)
      d_cm            implied mic spacing IF the dominant source were at endfire
                      (upper bound on spacing; broadside sources give ~0 here)
      f_alias_hz      c/(2d) using a nominal d if given, else None
      gain_db         20log10(rms_L/rms_R) - level mismatch to null with mic gain
      broadside_frac  fraction of voiced frames whose per-frame |TDOA| < 0.25 sample
                      (how "in front / centred" the capture already is)
      tdoa_hist       (centers_samples, counts) per-frame TDOA histogram
    """
    tau, _, _ = gcc_phat(left, right, fs, max_tau=0.002)
    rms_l = _rms(left)
    rms_r = _rms(right)
    gain_db = 20 * np.log10((rms_l + 1e-12) / (rms_r + 1e-12))

    # Per-frame TDOA distribution (only on frames with energy)
    n = int(frame_ms * fs / 1000)
    hop = n // 2
    thr = 0.15 * max(rms_l, rms_r)
    taus = []
    for start in range(0, len(left) - n, hop):
        fl = left[start : start + n]
        fr = right[start : start + n]
        if _rms(fl) < thr:
            continue
        t, _, _ = gcc_phat(fl, fr, fs, max_tau=0.002)
        taus.append(t * fs)  # in samples
    taus = np.asarray(taus)
    if len(taus):
        broadside_frac = float(np.mean(np.abs(taus) < 0.25))
        median_tdoa = float(np.median(taus))
        iqr_tdoa = float(np.subtract(*np.percentile(taus, [75, 25])))
        counts, edges = np.histogram(taus, bins=np.arange(-16, 16.5, 0.5))
        centers = 0.5 * (edges[:-1] + edges[1:])
    else:
        broadside_frac = median_tdoa = iqr_tdoa = float("nan")
        centers, counts = np.array([]), np.array([])

    return {
        # Global GCC-PHAT is unreliable on quasi-periodic voice (it can lock onto a
        # pitch-period lag), so the robust per-frame MEDIAN is the headline; the
        # global value is kept only as a caveated cross-check. For a clean spacing
        # measurement, use an endfire transient (finger snap from the side).
        "delay_samples": median_tdoa,
        "delay_global": tau * fs,
        "iqr_tdoa": iqr_tdoa,
        "d_cm": abs(median_tdoa) * C_SOUND / fs * 100.0,
        "gain_db": gain_db,
        "broadside_frac": broadside_frac,
        "tdoa_hist": (centers, counts),
    }


def calibrate_apply(left, right, delay_samples=0.0, gain_db=0.0):
    """Undo a measured mic mismatch: shift R by -delay and scale its level so a
    broadside target lines up. delay/gain come from estimate_geometry (measured
    against a *known broadside* reference for best results)."""
    r = frac_delay(right, -delay_samples)
    r = r * (10.0 ** (gain_db / 20.0))
    n = min(len(left), len(r))
    return left[:n].copy(), r[:n]


# ---------------------------------------------------------------------------
# Linear front-ends
# ---------------------------------------------------------------------------


def beam_sum(left, right):
    """Fixed broadside delay-sum beam (here just the average, since the target
    has zero steering delay). MVDR-optimal for a broadside target in isotropic
    noise; the artifact-free linear baseline everything else improves on."""
    n = min(len(left), len(right))
    return 0.5 * (left[:n] + right[:n])


def beam_diff(left, right):
    """Target-free reference (dipole with its null on broadside)."""
    n = min(len(left), len(right))
    return 0.5 * (left[:n] - right[:n])


# ---------------------------------------------------------------------------
# Method A: SUM + NLMS adaptive canceller driven by DIFF  (GSC-lite)
# ---------------------------------------------------------------------------


def gsc_lite(left, right, taps=32, mu=0.5, leak=1e-4, freeze_ratio=8.0,
             band=(250.0, 6500.0), fs=16000, model_delay=None):
    """Generalized-sidelobe-canceller-lite.

    primary   = SUM  (target + leaked noise)
    reference = DIFF (target-free: broadside is a perfect null of L-R)
    An NLMS FIR predicts the noise in primary from reference and subtracts it.
    Because the look direction is fixed at broadside, the blocker is exact and
    there is no target-cancellation from steering error (the classic GSC failure).

    freeze_ratio: when the primary is much stronger than the reference the frame
    is target-dominated -> freeze adaptation (a cheap double-talk guard) so we
    never adapt a null onto the wearer's own voice.
    """
    # Optionally band-limit BOTH paths to the voice band so the canceller both
    # adapts and subtracts consistently within the band it cares about.
    s = beam_sum(left, right).astype(np.float64)
    u = beam_diff(left, right).astype(np.float64)
    if band is not None:
        sos = signal.butter(4, [band[0], band[1]], btype="band", fs=fs, output="sos")
        s = signal.sosfilt(sos, s)
        u = signal.sosfilt(sos, u)

    # The ideal DIFF->SUM noise transfer is non-causal, so delay the primary by
    # ~half the filter length: the causal FIR then straddles the reference (it
    # sees reference samples both before and, relative to the delayed primary,
    # after the target sample) and can realise a centered filter. Without this a
    # strictly-causal canceller falls far short of the achievable cancellation.
    D = taps // 2 if model_delay is None else model_delay
    if D:
        s = np.concatenate([np.zeros(D), s])[: len(u)]

    n = len(u)
    w = np.zeros(taps)
    out = np.zeros(n)
    ubuf = np.zeros(taps)
    p_s = 1e-6      # smoothed SUM power (target+noise)
    p_u = 1e-6      # smoothed DIFF power (noise reference)
    a = 0.98
    for i in range(n):
        ubuf[1:] = ubuf[:-1]
        ubuf[0] = u[i]
        p_s = a * p_s + (1 - a) * s[i] * s[i]
        p_u = a * p_u + (1 - a) * u[i] * u[i]
        e = s[i] - float(w @ ubuf)
        out[i] = e
        # Double-talk guard: only adapt when the reference carries real energy
        # (a lateral source is present) and the frame is NOT target-dominated
        # (p_s not hugely larger than p_u). This protects the wearer's own voice
        # if mic mismatch lets a little target leak into the DIFF reference.
        if p_u > 1e-6 and p_s < (freeze_ratio ** 2) * p_u:
            norm = float(ubuf @ ubuf) + 1e-6
            w = (1 - leak) * w + (mu * e / norm) * ubuf
    # out[i] cleaned the D-delayed primary; advance it back onto the input
    # timeline so it stays sample-aligned with L/R (and any reference metric).
    if D:
        out = np.concatenate([out[D:], np.zeros(D)])
    return out.astype(np.float32)


# ---------------------------------------------------------------------------
# STFT helpers
# ---------------------------------------------------------------------------


def _stft(x, fs, nfft=256, hop=128):
    f, t, Z = signal.stft(x, fs=fs, window="hann", nperseg=nfft, noverlap=nfft - hop,
                          nfft=nfft, boundary="zeros", padded=True)
    return f, t, Z


def _istft(Z, fs, nfft=256, hop=128):
    _, x = signal.istft(Z, fs=fs, window="hann", nperseg=nfft, noverlap=nfft - hop,
                        nfft=nfft)
    return x.astype(np.float32)


def _smooth_time(X, alpha=0.7):
    """1-pole recursive average along the time (frame) axis of an STFT."""
    Y = np.empty_like(X)
    acc = X[:, 0]
    for t in range(X.shape[1]):
        acc = alpha * acc + (1 - alpha) * X[:, t]
        Y[:, t] = acc
    return Y


def _voice_band(f, lo=250.0, hi=6500.0, edge=100.0):
    """Raised-cosine voice-band weighting over the STFT bin frequencies."""
    g = np.ones_like(f)
    g *= 0.5 * (1 - np.cos(np.pi * np.clip((f - (lo - edge)) / edge, 0, 1)))
    g *= 0.5 * (1 + np.cos(np.pi * np.clip((f - hi) / edge, 0, 1)))
    return g


# ---------------------------------------------------------------------------
# Method B: IPD / ILD phase-alignment soft mask
# ---------------------------------------------------------------------------


def ipd_ild_mask(left, right, fs, d_cm=10.0, nfft=256, hop=128,
                 accept_sin=0.5, sigma_ild_db=6.0, smooth=0.6,
                 floor_db=-15.0, band=(250.0, 6500.0)):
    """Per-T-F soft mask that passes only broadside, level-matched, voice-band bins.

    accept_sin: half-beamwidth as |sin(theta)| (0.5 => +/-30deg). The IPD tolerance
      is made frequency-dependent (sigma_ipd(f) = 2*pi*f*d*accept_sin/c) so the
      spatial beam has a constant angular width. Above the spatial-aliasing
      frequency the IPD wraps and is ambiguous -> the IPD term is disabled there
      and we lean on ILD + the voice band.
    Returns (out, mask, meta).
    """
    d = d_cm / 100.0
    fL, t, ZL = _stft(left, fs, nfft, hop)
    _, _, ZR = _stft(right, fs, nfft, hop)

    sxy = ZL * np.conj(ZR)
    ipd = np.angle(sxy)  # instantaneous inter-channel phase diff, per bin
    ild_db = 20 * np.log10((np.abs(ZL) + 1e-9) / (np.abs(ZR) + 1e-9))

    f = fL[:, None]
    f_alias = C_SOUND / (2 * d)
    sigma_ipd = 2 * np.pi * f * d * accept_sin / C_SOUND  # radians, grows with f
    sigma_ipd = np.clip(sigma_ipd, 0.15, np.pi)
    mask_ipd = np.exp(-(ipd / sigma_ipd) ** 2)
    mask_ipd[fL >= f_alias, :] = 1.0  # ambiguous above aliasing -> don't gate on IPD

    mask_ild = np.exp(-(ild_db / sigma_ild_db) ** 2)
    band_g = _voice_band(fL, band[0], band[1])[:, None]

    mask = band_g * mask_ipd * mask_ild
    mask = _smooth_time(mask, smooth)
    floor = 10 ** (floor_db / 20.0)
    mask = np.clip(mask, floor, 1.0)

    Y = mask * 0.5 * (ZL + ZR)
    out = _istft(Y, fs, nfft, hop)
    return out, mask, {"f_alias_hz": f_alias, "freqs": fL, "times": t}


# ---------------------------------------------------------------------------
# Method C: coherent-to-diffuse ratio (CDR) mask
# ---------------------------------------------------------------------------


def diffuse_coherence(f, d_cm):
    """Spatial coherence of an isotropic (diffuse) field between two omnis:
    Gamma_diff(f) = sinc(2 f d / c)  [np.sinc(x)=sin(pi x)/(pi x)]."""
    d = d_cm / 100.0
    return np.sinc(2 * f * d / C_SOUND)


def cdr_mask(left, right, fs, d_cm=10.0, nfft=256, hop=128, smooth=0.75,
             floor_db=-15.0, band=(250.0, 6500.0), overest=1.3):
    """Suppress diffuse energy, keep coherent (directional) energy.

    Uses the magnitude-squared coherence (MSC) relative to the *known* diffuse
    floor. Where the measured MSC sits at the diffuse model, the bin is noise
    (gain -> 0); where it approaches 1, the bin is a coherent source (gain -> 1):

        MSC     = |E{XL XR*}|^2 / (E|XL|^2 E|XR|^2)     (smoothed estimates)
        MSC_dif = |Gamma_diff(f)|^2
        G       = clip((MSC - overest*MSC_dif) / (1 - overest*MSC_dif), 0, 1)

    This is a simplified, DOA-free coherent-to-diffuse-ratio gain (cf. Schwarz &
    Kellermann 2015); it rejects babble, wind and reverberant tails regardless of
    direction, and pairs with method B (which adds the *broadside* selectivity).
    """
    fL, t, ZL = _stft(left, fs, nfft, hop)
    _, _, ZR = _stft(right, fs, nfft, hop)

    sxx = _smooth_time(np.abs(ZL) ** 2, smooth)
    syy = _smooth_time(np.abs(ZR) ** 2, smooth)
    sxy = _smooth_time(ZL * np.conj(ZR), smooth)
    msc = (np.abs(sxy) ** 2) / (sxx * syy + 1e-12)

    msc_dif = (np.abs(diffuse_coherence(fL, d_cm)) ** 2)[:, None]
    g = (msc - overest * msc_dif) / (1 - overest * msc_dif + 1e-9)
    g = np.clip(g, 0.0, 1.0)

    band_g = _voice_band(fL, band[0], band[1])[:, None]
    mask = np.clip(band_g * g, 10 ** (floor_db / 20.0), 1.0)

    Y = mask * 0.5 * (ZL + ZR)
    out = _istft(Y, fs, nfft, hop)
    return out, mask, {"freqs": fL, "times": t}


def combined_mask(left, right, fs, d_cm=10.0, **kw):
    """Recommended default: multiply the broadside (B) and coherent (C) masks.

    B answers 'is this bin coming from straight ahead?', C answers 'is this bin a
    real directional source or just diffuse noise?'. Their product keeps only
    broadside coherent voice.
    """
    nfft = kw.get("nfft", 256)
    hop = kw.get("hop", 128)
    band = kw.get("band", (250.0, 6500.0))
    _, mb, meta = ipd_ild_mask(left, right, fs, d_cm=d_cm, nfft=nfft, hop=hop,
                               band=band, floor_db=-40.0)
    _, mc, _ = cdr_mask(left, right, fs, d_cm=d_cm, nfft=nfft, hop=hop, band=band,
                        floor_db=-40.0)
    mask = np.clip(mb * mc, 10 ** (kw.get("floor_db", -15.0) / 20.0), 1.0)
    fL, _, ZL = _stft(left, fs, nfft, hop)
    _, _, ZR = _stft(right, fs, nfft, hop)
    Y = mask * 0.5 * (ZL + ZR)
    return _istft(Y, fs, nfft, hop), mask, meta


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------


def _rms(x):
    return float(np.sqrt(np.mean(np.square(x)) + 1e-20))


def segmental_snr(estimate, target_ref, fs, frame_ms=20):
    """Segmental SNR of `estimate` against a known clean `target_ref` (requires the
    synthetic scene, which provides ground truth). Higher = closer to clean target."""
    n = min(len(estimate), len(target_ref))
    est, ref = estimate[:n], target_ref[:n]
    # scale-invariant: project est onto ref
    a = np.dot(est, ref) / (np.dot(ref, ref) + 1e-12)
    err = est - a * ref
    fl = int(frame_ms * fs / 1000)
    snrs = []
    for i in range(0, n - fl, fl):
        r = ref[i : i + fl]
        e = err[i : i + fl]
        if _rms(r) < 1e-4:
            continue
        snrs.append(10 * np.log10((np.dot(r, r) + 1e-12) / (np.dot(e, e) + 1e-12)))
    return float(np.mean(snrs)) if snrs else float("nan")


def level_db(x):
    return 20 * np.log10(_rms(x) + 1e-12)
