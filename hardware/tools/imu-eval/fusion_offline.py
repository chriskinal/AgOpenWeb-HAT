#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors
"""Offline tilt filters on a fusion-off MEMS Studio log (low-g, high-g, gyro), the first step of
the daemon's own fusion (hardware/imu-breakout/HARDWARE_IMU_BREAKOUT.md section 8.5, K8): which
accel, averaged how, holds roll and pitch through the shaking.

    python3 fusion_offline.py ST_LOG.csv [--still0 S --still1 S] [--after S]

Each filter is Mahony-type on the gravity vector in the body frame: turned by the gyro each sample,
pulled toward the chosen accel (kp = 1/tau), the pull's integral re-learning the gyro bias while
the board is not shaking. Gyro bias and the high-g's offset from the low-g are learnt over the
still stretch at the start (--still0 to --still1), as the daemon would at standstill; the last
--after seconds, still again, give the high-g's offset at the end, for the temperature variant.
Errors are against the tilt at rest at the start: the plate does not tilt while it shakes.
"""
import argparse, math, os, statistics as st, sys

ap = argparse.ArgumentParser()
ap.add_argument("log")
ap.add_argument("--still0", type=float, default=20)
ap.add_argument("--still1", type=float, default=65)
ap.add_argument("--after", type=float, default=50)
ap.add_argument("--shake", type=float, nargs=2, metavar=("FROM", "TO"),
                help="the stretch to score (default: the longest over 100 mg rms)")
a = ap.parse_args()
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # analyze.py
import analyze as A

S = A.load_st(a.log)
t = S["t"]
L = [S[k] for k in A.ACC]
H = [S[k] for k in A.ACC_H]
G = [S[k] for k in A.GYRO]
n = len(t)
still = [i for i in range(n) if a.still0 <= t[i] < a.still1]
bias = [st.mean(g[i] for i in still) for g in G]
lmean = [st.mean(c[i] for i in still) for c in L]
hoff = [st.mean(H[k][i] for i in still) - lmean[k] for k in range(3)]  # high-g minus low-g at rest
truth = A.tilt(*lmean)

# Shaking detector: low-g rms about its 0.5 s mean, from consecutive 0.5 s windows.
shake = [0.0] * n
i = 0
while i < n:
    j = i
    while j < n and t[j] < t[i] + 0.5:
        j += 1
    r = math.sqrt(sum(st.pvariance(c[i:j]) for c in L)) if j - i > 2 else 0.0
    for q in range(i, j):
        shake[q] = r
    i = j


def run(tau, pick, ti=None, learn=lambda i: 1.0):
    """pick(i) -> accel vector in mg to pull toward, or None to coast on the gyro. With ti, a
    Mahony filter: the pull goes in as a rate (kp = 1/tau) and its integral (time constant ti)
    re-learns the gyro bias at the rate learn(i) (0 to 1). pick may return (vector, gain) to scale
    the pull for that sample."""
    v = list(lmean)
    b = [0.0, 0.0, 0.0]  # bias learnt on top of the standstill one, rad/s
    kp, ki = 1.0 / tau, (1.0 / (tau * ti) if ti else 0.0)
    out = []
    nxt = 0.0
    for i in range(1, n):
        dt = t[i] - t[i - 1]
        w = [math.radians((G[k][i] - bias[k]) / 1000) - b[k] for k in range(3)]
        m = pick(i)
        g = 1.0
        if m is not None and len(m) == 2:
            m, g = m
        if m is not None:
            na, nv = math.sqrt(sum(x * x for x in m)), math.sqrt(sum(x * x for x in v))
            an, vn = [x / na for x in m], [x / nv for x in v]
            e = [an[1] * vn[2] - an[2] * vn[1], an[2] * vn[0] - an[0] * vn[2], an[0] * vn[1] - an[1] * vn[0]]
            w = [w[k] + g * kp * e[k] for k in range(3)]
            if ki:
                b = [b[k] - learn(i) * g * ki * e[k] * dt for k in range(3)]
        # world-fixed vector seen from a body turning at w: dv/dt = v x w
        cx = v[1] * w[2] - v[2] * w[1]
        cy = v[2] * w[0] - v[0] * w[2]
        cz = v[0] * w[1] - v[1] * w[0]
        v = [v[0] + cx * dt, v[1] + cy * dt, v[2] + cz * dt]
        if t[i] >= nxt:
            out.append((t[i],) + A.tilt(*v))
            nxt = t[i] + 1.0
    return out


def lowpass(cols, tau):
    """First-order low-pass of each column, in place of the raw samples: linear, so a vibration
    averages out before anything nonlinear (normalising, the cross product) sees it."""
    out = []
    for c in cols:
        y, o = c[0], [c[0]] * n
        for i in range(1, n):
            y += (t[i] - t[i - 1]) / tau * (c[i] - y)
            o[i] = y
        out.append(o)
    return out


THR = 100.0  # mg rms: shaking
quiet = lambda i: 1.0 if shake[i] <= THR else 0.0
Hc = [[x - hoff[k] for x in H[k]] for k in range(3)]
Lf, Hf = lowpass(L, 0.5), lowpass(Hc, 0.5)
vec = lambda C: (lambda i: (C[0][i], C[1][i], C[2][i]))
switch = lambda A_, B_: (lambda i: vec(B_)(i) if shake[i] > THR else vec(A_)(i))
# High-g offset against temperature, from the still stretches at the start and the end (what a
# unit would learn over its standstills). Fitted on the same run: a best case.
T_ = S["temp[C]"]
end_still = [i for i in range(n) if t[-1] - a.after <= t[i] < t[-1] - 5]
def off_at(idx):
    return [st.mean(H[k][i] - L[k][i] for i in idx) for k in range(3)], st.mean(T_[i] for i in idx)
(o0, T0), (o1, T1) = off_at(still), off_at(end_still)
print("high-g minus low-g: %.2f C %s, %.2f C %s mg" % (T0, ["%+.1f" % x for x in o0], T1, ["%+.1f" % x for x in o1]))
Ht = [[H[k][i] - (o0[k] + (o1[k] - o0[k]) * (T_[i] - T0) / (T1 - T0)) for i in range(n)] for k in range(3)]
Htf = lowpass(Ht, 0.5)
# Smooth shaking measure: the low-g's rms about its own 0.5 s low-pass, itself low-passed 0.5 s.
dev2 = [[(L[k][i] - Lf[k][i]) ** 2 for i in range(n)] for k in range(3)]
rms_s = [math.sqrt(sum(x)) for x in zip(*lowpass(dev2, 0.5))]


def blend(half, Hsrc, slow=None):
    """Gravity from w x low-g + (1 - w) x high-g, w = 1 / (1 + (rms / half)^2): all low-g when
    quiet, half-and-half at rms = half, high-g above. With slow (mg), the pull also weakens as the
    shaking grows (gain 1 / (1 + rms / slow)), leaning on the gyro."""
    def pick(i):
        r = rms_s[i]
        w = 1.0 / (1.0 + (r / half) ** 2)
        v = tuple(w * Lf[k][i] + (1 - w) * Hsrc[k][i] for k in range(3))
        return (v, 1.0 / (1.0 + r / slow)) if slow else v
    return pick


learn_w = lambda half: (lambda i: 1.0 / (1.0 + (rms_s[i] / half) ** 2))

variants = [
    ("gyro only", None, None, None),
    ("low-g raw, 1 s", 1.0, vec(L), None),
    ("low-g 0.5 s lp, 1 s", 1.0, vec(Lf), None),
    ("switch raw, 2 s", 2.0, switch(L, Hc), 30.0),
    ("switch lp, 2 s", 2.0, switch(Lf, Hf), 30.0),
    ("switch lp, 5 s", 5.0, switch(Lf, Hf), 30.0),
    ("switch lp+temp, 2 s", 2.0, switch(Lf, Htf), 30.0),
    ("blend 50 mg, 2 s", 2.0, blend(50, Hf), 30.0, learn_w(50)),
    ("blend 100 mg, 2 s", 2.0, blend(100, Hf), 30.0, learn_w(100)),
    ("blend 50+slow, 2 s", 2.0, blend(50, Hf, 200), 30.0, learn_w(50)),
    ("blend 50+temp, 2 s", 2.0, blend(50, Htf), 30.0, learn_w(50)),
    ("blend 50+temp+slow", 2.0, blend(50, Htf, 200), 30.0, learn_w(50)),
]
res = {}
for name, tau, pick, ti, *lw in variants:
    res[name] = run(tau or 1.0, pick or (lambda i: None), ti, lw[0] if lw else quiet)

# The shaking: the longest stretch over the threshold (not the taps).
runs, start = [], None
for x, sh in zip(t, shake):
    if sh > THR and start is None:
        start = x
    elif sh <= THR and start is not None:
        runs.append((start, x)); start = None
s0, s1 = a.shake if a.shake else max(runs, key=lambda r: r[1] - r[0])
print("truth (still at the start): roll %+.3f pitch %+.3f; high-g offset %+.1f %+.1f %+.1f mg" % (truth + tuple(hoff)))
print("shaking %.1f-%.1f s (%s)\n" % (s0, s1, "given" if a.shake else "low-g rms over %.0f mg" % THR))
print("%-20s %22s %22s %22s" % ("", "worst while shaking", "end of shaking", "60 s after"))
for name, *_ in variants:
    o = res[name]
    dur = [(x, r - truth[0], p - truth[1]) for x, r, p in o if s0 <= x <= s1]
    worst = max(dur, key=lambda e: math.hypot(e[1], e[2]))
    end = dur[-1]
    after = min(o, key=lambda e: abs(e[0] - (s1 + 60)))
    print("%-20s %+9.3f %+9.3f    %+9.3f %+9.3f    %+9.3f %+9.3f" % (
        name, worst[1], worst[2], end[1], end[2], after[1] - truth[0], after[2] - truth[1]))

print("\nroll / pitch error every 10 s:")
print("%6s " % "t" + " ".join("%-17s" % v[0] for v in variants))
for idx in range(0, len(res["gyro only"]), 10):
    row = []
    for name, *_ in variants:
        x, r, p = res[name][idx]
        row.append("%+7.2f %+7.2f  " % (r - truth[0], p - truth[1]))
    print("%6.0f " % res["gyro only"][idx][0] + " ".join(row))
