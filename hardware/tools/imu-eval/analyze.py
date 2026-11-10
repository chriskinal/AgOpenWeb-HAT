#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors
"""Analyses an LSM6DSK320X log from MEMS Studio against a TM171 log from field_log.py
(hardware/imu-breakout/HARDWARE_IMU_BREAKOUT.md section 8).

    python3 analyze.py ST_LOG.csv [TM171_LOG.csv] [--still] [--allan-csv FILE]

Lines the two logs up on the first and last tap (a gyro spike in both) or, when the TM171 misses
the taps, on the shaking itself (the accel spread, matched in each half of the run), then reports:
  - rates, ranges hit (clipping), temperature
  - per segment (the TM171 log's markers, or still / shaking found from the accel and gyro): the
    mean and spread of roll and pitch for each IMU, as a shift from the first segment; the
    vibration dose (accel rms about the segment's mean), peaks, clipped samples and gyro bias shift
  - with --still (a run at rest): drift of the gyro bias, accel offset and tilt in 10-minute
    blocks and against temperature, and the Allan deviation (ST DT0064) of each gyro and accel axis

Axes and signs as the ST part (X forward, Y left, Z up; accel reads +1 g on Z when level). The
TM171 has the same axes; its accel has the opposite sign and its gyro is in rad/s. Roll is
positive left side up, pitch positive nose up. MEMS Studio's own "roll" is our pitch and its
"pitch" is minus our roll, so tilt is taken from its gravity vector (fusion on) or the accel.
Pure Python, no numpy: runs where pyserial runs.
"""
import argparse, csv, math, statistics as st
from array import array

GYRO = ("gyro_x[mdps]", "gyro_y[mdps]", "gyro_z[mdps]")
ACC = ("acc_l_x[mg]", "acc_l_y[mg]", "acc_l_z[mg]")
ACC_H = ("acc_h_x[mg]", "acc_h_y[mg]", "acc_h_z[mg]")
GRAV = ("gravity_x[mg]", "gravity_y[mg]", "gravity_z[mg]")
BIAS = ("bias_gyr_x[mdps]", "bias_gyr_y[mdps]", "bias_gyr_z[mdps]")


def tilt(x, y, z):
    return math.degrees(math.atan2(y, z)), math.degrees(math.atan2(x, math.hypot(y, z)))


class Log:
    """Columns as arrays: t (s from the first row) plus whatever is asked for."""

    def __init__(self, name):
        self.name, self.col = name, {}

    def __getitem__(self, k):
        return self.col[k]

    def __contains__(self, k):
        return k in self.col

    def __len__(self):
        return len(self.col["t"])


def load_st(path):
    with open(path, newline="") as f:
        for line in f:
            if line.startswith("time["):
                hdr = line.strip().split(",")
                break
        else:
            raise SystemExit("%s: no MEMS Studio header" % path)
        want = [k for k in ("temp[C]",) + GYRO + ACC + ACC_H + GRAV + BIAS if k in hdr]
        idx = [hdr.index(k) for k in want]
        log = Log("ST")
        log.col = {k: array("d") for k in ["t"] + want}
        t0 = None
        # The ranges, from the mg per LSB of the first row that has them.
        log.fs = {}
        sens = {"acc_l": {0.061: 2, 0.122: 4, 0.244: 8, 0.488: 16},
                "acc_h": {0.976: 32, 1.952: 64, 3.904: 128, 7.808: 256, 9.76: 320},
                "gyro": {4.375: 125, 8.75: 250, 17.5: 500, 35.0: 1000, 70.0: 2000, 140.0: 4000}}
        for r in csv.reader(f):
            if len(r) < len(hdr):
                continue
            for part in sens:
                if part not in log.fs and part + "_x[LSB]" in hdr:
                    phys = [h for h in hdr if h.startswith(part + "_x[") and "LSB" not in h][0]
                    lsb = float(r[hdr.index(part + "_x[LSB]")])
                    if abs(lsb) >= 4:
                        ratio = float(r[hdr.index(phys)]) / lsb
                        log.fs[part] = min(sens[part].items(), key=lambda kv: abs(kv[0] - ratio))[1]
            t = float(r[0])
            if t0 is None:
                t0 = t
            log["t"].append((t - t0) / 1e6)
            for k, i in zip(want, idx):
                log[k].append(float(r[i]))
    if "acc_h_x[mg]" in log and not any(log["acc_h_x[mg]"]):
        for k in ACC_H:
            del log.col[k]
    return log


def load_tm(path):
    log = Log("TM171")
    keys = ("roll", "pitch", "yaw", "gx", "gy", "gz", "ax", "ay", "az", "temp")
    log.col = {k: array("d") for k in ("t", "marker") + keys}
    prev, wraps, t0 = None, 0, None
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if not all(r[k] for k in keys[:9]):
                continue
            ts = int(r["dev_ts"])
            if prev is not None and ts < prev:
                wraps += 1  # a 32-bit microsecond counter: wraps every 71.6 min
            prev = ts
            t = (ts + wraps * 2 ** 32) / 1e6
            if t0 is None:
                t0 = t
            log["t"].append(t - t0)
            log["marker"].append(float(r["marker"]))
            for k in keys:
                v = float(r[k]) if r[k] else math.nan
                if k in ("gx", "gy", "gz"):
                    v = math.degrees(v) * 1000  # rad/s to mdps
                elif k in ("ax", "ay", "az"):
                    v = -v * 1000  # g, gravity sign, to mg as the ST part
                log[k].append(v)
    return log


def rate_mag(log, keys):
    return [math.sqrt(a * a + b * b + c * c) / 1000 for a, b, c in zip(*(log[k] for k in keys))]


def bursts(t, g, thr, gap=0.5):
    out = []
    for ti, gi in zip(t, g):
        if gi > thr:
            if out and ti - out[-1][1] < gap:
                out[-1][1] = ti
                out[-1][2] = max(out[-1][2], gi)
            else:
                out.append([ti, ti, gi])
    return out


TM_PPM = 2650  # the TM171's clock, fast, as measured on the taps of the earlier runs


def envelope(t, cols, w=0.2):
    """Accel spread in w-second bins: log10(1 + sd in mg), indexed by bin."""
    out, i, n = {}, 0, len(t)
    while i < n:
        b = int(t[i] // w)
        j = i
        while j < n and int(t[j] // w) == b:
            j += 1
        if j - i > 2:
            out[b] = math.log10(1 + math.sqrt(sum(st.pvariance(c[i:j]) for c in cols)))
        i = j
    return out


def match(es, et, lo, hi, w=0.2, reach=30.0):
    """The shift (s) of the TM171 envelope that best matches the ST one between lo and hi."""
    best = None
    bins = [b for b in range(int(lo / w), int(hi / w)) if b in es]
    for d in range(-int(reach / w), int(reach / w) + 1):
        pairs = [(es[b], et[b - d]) for b in bins if b - d in et]
        if len(pairs) < 50:
            continue
        x, y = zip(*pairs)
        sx, sy = st.pstdev(x), st.pstdev(y)
        if not sx or not sy:
            continue
        mx, my = st.mean(x), st.mean(y)
        r = sum((a - mx) * (b - my) for a, b in pairs) / (len(pairs) * sx * sy)
        if best is None or r > best[1]:
            best = (d * w, r)
    return best


def window(t, v, lo, hi):
    return [x for ti, x in zip(t, v) if lo <= ti < hi and not math.isnan(x)]


def allan(y, tau0):
    """Overlapping Allan deviation of rate samples y at spacing tau0, at octave taus."""
    theta = array("d", [0.0])
    s = 0.0
    for v in y:
        s += v * tau0
        theta.append(s)
    n = len(theta) - 1
    out = []
    m = 1
    while 2 * m < n // 4:
        step = max(1, m // 8)  # thin the overlap at long taus: same answer, a fraction of the time
        acc, cnt = 0.0, 0
        for k in range(0, n - 2 * m, step):
            d = theta[k + 2 * m] - 2 * theta[k + m] + theta[k]
            acc += d * d
            cnt += 1
        out.append((m * tau0, math.sqrt(acc / (2 * cnt)) / (m * tau0)))
        m *= 2
    return out


def active(S, lo, hi, win=0.5):
    """Stretches of vibration or motion between lo and hi: 0.5 s windows whose accel sd is over
    10 mg (still: under 1 mg) or whose gyro passes 10 dps, joined across gaps under 2 s, 2 s or longer."""
    out, t, i, n = [], S["t"], 0, len(S)
    while i < n and t[i] < lo:
        i += 1
    while i < n and t[i] < hi:
        j = i
        while j < n and t[j] < t[i] + win:
            j += 1
        sd = math.sqrt(sum(st.pvariance(S[k][i:j]) for k in ACC)) if j - i > 2 else 0.0
        g = max(math.sqrt(sum(S[k][m] ** 2 for k in GYRO)) for m in range(i, j)) / 1000 if j > i else 0.0
        if sd > 10 or g > 10:
            if out and t[i] - out[-1][1] < 2:
                out[-1][1] = t[j - 1]
            else:
                out.append([t[i], t[j - 1]])
        i = j
    return [(a, b) for a, b in out if b - a >= 2]


def levels(S, lo, hi, blk=5.0, step=1.5):
    """Where the shaking changes level inside a stretch (idle to working revs): the edges between
    5 s blocks whose accel rms differs by 1.5 times or more and stays there for the next block."""
    rms, t, i, n = [], S["t"], 0, len(S)
    while i < n and t[i] < lo:
        i += 1
    while i < n and t[i] < hi:
        j = i
        while j < n and t[j] < t[i] + blk and t[j] < hi:
            j += 1
        if j - i > 2:
            rms.append((t[i], math.sqrt(sum(st.pvariance(S[k][i:j]) for k in ACC))))
        i = j
    edges, base = [], rms[0][1] if rms else 0
    for b in range(1, len(rms) - 1):
        r, nxt = rms[b][1], rms[b + 1][1]
        up = r >= base * step and nxt >= base * step
        down = r <= base / step and nxt <= base / step
        if up or down:
            edges.append(rms[b][0])
            base = nxt
    return edges


def fit(x, y):
    mx, my = st.mean(x), st.mean(y)
    sxx = sum((a - mx) ** 2 for a in x)
    return (sum((a - mx) * (b - my) for a, b in zip(x, y)) / sxx) if sxx else 0.0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("st_log")
    ap.add_argument("tm_log", nargs="?")
    ap.add_argument("--still", action="store_true", help="a run at rest: drift and Allan deviation")
    ap.add_argument("--allan-csv", help="write the Allan deviation curves to this file")
    a = ap.parse_args()

    S = load_st(a.st_log)
    span = S["t"][-1]
    dts = [b - c for b, c in zip(S["t"][1:1001], S["t"][:1000])]
    tau0 = st.median(dts)
    print("ST: %d rows over %.1f s (%.1f min), %.1f Hz; fusion %s; high-g %s" % (
        len(S), span, span / 60, 1 / tau0, "on" if GRAV[0] in S else "off",
        "on" if ACC_H[0] in S else "off"))
    for k in GYRO + ACC:
        lo, hi = min(S[k]), max(S[k])
        print("  %-13s min %10.1f  max %10.1f" % (k, lo, hi))
    print("  ranges: %s" % ", ".join("%s +-%d" % (k, v) for k, v in S.fs.items()))
    if "temp[C]" in S:
        print("  temperature %.2f to %.2f C" % (min(S["temp[C]"]), max(S["temp[C]"])))

    T = load_tm(a.tm_log) if a.tm_log else None
    sg = rate_mag(S, GYRO)
    bs = bursts(S["t"], sg, 10)
    if T:
        tg = rate_mag(T, ("gx", "gy", "gz"))
        bt = bursts(T["t"], tg, 5)
        print("TM171: %d rows over %.1f s, temperature %.1f to %.1f C" % (
            len(T), T["t"][-1], min(T["temp"]), max(T["temp"])))
        k = (bs[-1][0] - bs[0][0]) / (bt[-1][0] - bt[0][0]) if len(bs) >= 2 and len(bt) >= 2 else 0
        if abs(1 / k - 1) < 0.01 if k else False:
            off = bs[0][0] - bt[0][0] * k
            print("  aligned on the taps at ST %.2f s and %.2f s; TM171 clock %+.0f ppm" % (
                bs[0][0], bs[-1][0], (1 / k - 1) * 1e6))
        else:
            # No pair of taps in both logs (the TM171's gyro is filtered and can miss them):
            # match the shaking instead, in each half, with the TM171's clock at its usual rate.
            k = 1 / (1 + TM_PPM * 1e-6)
            es = envelope(S["t"], [S[c] for c in ACC])
            et = envelope([x * k for x in T["t"]], [T[c] for c in ("ax", "ay", "az")])
            m1, m2 = match(es, et, 0, span / 2), match(es, et, span / 2, span)
            if m1 and m2 and m1[1] > 0.5 and m2[1] > 0.5:
                # The two halves give the rate too, when they are far enough apart to trust it.
                k2 = k * (1 + (m2[0] - m1[0]) / (span / 2))
                if abs(1 / k2 - 1) < 0.01 and span > 600:
                    k, off = k2, m1[0] - (m2[0] - m1[0]) / 2
                else:
                    off = (m1[0] + m2[0]) / 2
                print("  no taps in both logs: aligned on the shaking (match %.2f and %.2f), TM171 "
                      "starts %+.1f s, clock %+.0f ppm" % (m1[1], m2[1], off, (1 / k - 1) * 1e6))
            else:
                k, off = 1.0, 0.0
                print("  no taps in both logs and no shaking to match: lined up on the first rows only")
        T.col["t"] = array("d", (x * k + off for x in T["t"]))

    # Tilt for each IMU: the fused gravity vector if there is one, else the accel.
    src = GRAV if GRAV[0] in S else ACC
    S.col["roll"], S.col["pitch"] = (array("d", v) for v in zip(*(tilt(*x) for x in zip(*(S[k] for k in src)))))
    if T:
        T.col["acc_roll"], T.col["acc_pitch"] = (array("d", v) for v in zip(
            *(tilt(*x) for x in zip(T["ax"], T["ay"], T["az"]))))

    # Segments: between the taps, split at the TM171 markers or, without any, at motion.
    lo = bs[0][1] + 1 if len(bs) >= 2 else 0
    hi = bs[-1][0] - 1 if len(bs) >= 2 else span
    edges = [lo]
    if T and len(set(T["marker"])) > 1:
        edges += [t for t, m, pm in zip(T["t"][1:], T["marker"][1:], T["marker"]) if m != pm and lo < t < hi]
        how = "TM171 markers"
    else:
        for b0, b1 in active(S, lo, hi):
            edges += [b0] + levels(S, b0, b1) + [b1]
        how = "activity: accel sd over 10 mg or gyro over 10 dps, split where its level steps 1.5 times"
    edges.append(hi)
    print("\nSegments (%s), roll and pitch in degrees, shift from the first segment:" % how)
    print("  %-15s %-28s %-28s %-28s" % ("", "ST (%s)" % ("SFLP" if src is GRAV else "accel"),
                                        "TM171 fusion" if T else "", "TM171 accel" if T else ""))
    base = {}
    for i, (s0, s1) in enumerate(zip(edges, edges[1:])):
        cells = []
        for name, log, keys in (("S", S, ("roll", "pitch")), ("T", T, ("roll", "pitch")),
                                ("A", T, ("acc_roll", "acc_pitch"))):
            if not log:
                continue
            c = []
            for key in keys:
                v = window(log["t"], log[key], s0, s1)
                if not v:
                    c.append("     -     ")
                    continue
                m = st.mean(v)
                base.setdefault((name, key), m)
                c.append("%+6.3f sd %5.3f" % (m - base[(name, key)], st.pstdev(v)))
            cells.append(" ".join(c))
        print("  %6.1f-%6.1f s  %s" % (s0, s1, "  ".join(cells)))

    # Vibration dose and gyro bias per segment. Dose: accel minus the segment's mean (DT0106's
    # residual linear acceleration, with the mean standing in for gravity while the tilt holds).
    fs_l = S.fs.get("acc_l", 16) * 1000
    fs_h = S.fs.get("acc_h", 32) * 1000 if ACC_H[0] in S else None
    g_lim = 8.75 * S.fs.get("gyro", 250) / 250 * 32760  # mdps at the last LSB codes
    print("\nVibration and gyro per segment (low-g range +-%d g%s):" % (
        fs_l // 1000, ", high-g +-%d g" % (fs_h // 1000) if fs_h else ""))
    print("  %-15s %9s %9s %7s %8s %7s   %-24s %s" % ("", "rms g", "peak g", "clipped", "high-g pk", "gyro cl",
                                               "ST gyro shift mdps", "TM171 gyro shift mdps" if T else ""))
    gbase = {}
    for s0, s1 in zip(edges, edges[1:]):
        idx = [i for i, x in enumerate(S["t"]) if s0 <= x < s1]
        if not idx:
            continue
        means = {k: st.mean(S[k][i] for i in idx) for k in ACC + GYRO}
        rms = math.sqrt(st.mean(sum((S[k][i] - means[k]) ** 2 for k in ACC) for i in idx)) / 1000
        peak = max(abs(S[k][i]) for k in ACC for i in idx) / 1000
        clipped = sum(1 for i in idx if any(abs(S[k][i]) >= fs_l * 0.998 for k in ACC))
        gclip = sum(1 for i in idx if any(abs(S[k][i]) >= g_lim for k in GYRO))
        hpk = "%8.2f" % (max(abs(S[k][i]) for k in ACC_H for i in idx) / 1000) if fs_h else "       -"
        gs = []
        for k in GYRO:
            gbase.setdefault(k, means[k])
            gs.append("%+7.1f" % (means[k] - gbase[k]))
        tgs = []
        if T:
            for k in ("gx", "gy", "gz"):
                v = window(T["t"], T[k], s0, s1)
                if v:
                    gbase.setdefault("T" + k, st.mean(v))
                    tgs.append("%+7.1f" % (st.mean(v) - gbase["T" + k]))
        print("  %6.1f-%6.1f s %9.3f %9.2f %7d %s %7d   %-24s %s" % (
            s0, s1, rms, peak, clipped, hpk, gclip, " ".join(gs), " ".join(tgs)))
    print("  pass (section 8.3): roll shift <= 0.2 deg, gyro shift <= 100 mdps")

    if not a.still:
        return

    # Drift at rest: 10-minute blocks between the taps.
    print("\nDrift at rest, 10-minute blocks (ST gyro bias mdps, ST accel mg, tilt deg):")
    blk = 600.0
    rows = []
    t = lo
    while t + blk <= hi + 1:
        r = {"t": t}
        for k in GYRO + ACC + ("temp[C]", "roll", "pitch"):
            if k in S:
                r[k] = st.mean(window(S["t"], S[k], t, t + blk))
        if T:
            for k in ("roll", "pitch", "yaw", "temp"):
                r["T" + k] = st.mean(window(T["t"], T[k], t, t + blk))
        rows.append(r)
        t += blk
    print("  %6s %6s %8s %8s %8s %8s %8s %8s %7s %7s" % (
        "min", "temp", "gx", "gy", "gz", "ax", "ay", "az", "roll", "pitch") + (
        "   %6s %7s %7s %7s" % ("TMtemp", "TMroll", "TMpitch", "TMyaw") if T else ""))
    for r in rows:
        line = "  %6.0f %6.2f %8.1f %8.1f %8.1f %8.2f %8.2f %8.2f %7.3f %7.3f" % (
            r["t"] / 60, r.get("temp[C]", math.nan), *(r[k] for k in GYRO + ACC), r["roll"], r["pitch"])
        if T:
            line += "   %6.2f %7.3f %7.3f %7.2f" % (r["Ttemp"], r["Troll"], r["Tpitch"], r["Tyaw"])
        print(line)
    if "temp[C]" in S and len(rows) > 2 and max(r["temp[C]"] for r in rows) - min(r["temp[C]"] for r in rows) > 0.2:
        temps = [r["temp[C]"] for r in rows]
        print("  against ST temperature (fit over the blocks): gyro %s mdps/C, accel %s mg/C, tilt %s deg/C" % (
            " ".join("%+.1f" % fit(temps, [r[k] for r in rows]) for k in GYRO),
            " ".join("%+.3f" % fit(temps, [r[k] for r in rows]) for k in ACC),
            " ".join("%+.4f" % fit(temps, [r[k] for r in rows]) for k in ("roll", "pitch"))))
        print("  datasheet, typical: gyro +-6 mdps/C, accel +-0.07 mg/C")

    # Allan deviation of the raw rates between the taps.
    print("\nAllan deviation (overlapping), ST tau0 %.2f ms%s:" % (tau0 * 1000, ", TM171 20 ms" if T else ""))
    curves = {}
    i0 = next(i for i, x in enumerate(S["t"]) if x >= lo)
    i1 = next((i for i, x in enumerate(S["t"]) if x >= hi), len(S))
    for k in GYRO + ACC:
        curves["ST " + k] = allan(S[k][i0:i1], tau0)
    if T:
        j0 = next(i for i, x in enumerate(T["t"]) if x >= lo)
        j1 = next((i for i, x in enumerate(T["t"]) if x >= hi), len(T))
        tt = st.median([b - c for b, c in zip(T["t"][j0 + 1:j0 + 1001], T["t"][j0:j0 + 1000])])
        for k in ("gx", "gy", "gz", "ax", "ay", "az"):
            curves["TM " + k] = allan(T[k][j0:j1], tt)
    for name, c in curves.items():
        gyro = "gyro" in name or name[3] == "g"
        unit = "deg/h" if gyro else "mg"
        scale = 3.6 if gyro else 1.0  # mdps to deg/h
        # White noise: the value at tau = 1 s on the -1/2 slope. Floor: the minimum.
        at1 = min(c, key=lambda p: abs(math.log(p[0])))
        floor = min(c, key=lambda p: p[1])
        if gyro:
            white = "ARW %.3f deg/sqrt(h)" % (at1[1] / 1000 * math.sqrt(at1[0]) * 60)
        else:
            white = "VRW %.0f ug/sqrt(Hz)" % (at1[1] * 1000 * math.sqrt(at1[0]))
        print("  %-16s %s at 1 s; bias instability %.3f %s at %.0f s" % (
            name, white, floor[1] * scale / 0.664, unit, floor[0]))
    print("  datasheet, ST: rate noise 3.8 mdps/sqrt(Hz) = ARW 0.23 deg/sqrt(h); accel 60 ug/sqrt(Hz)")
    if a.allan_csv:
        with open(a.allan_csv, "w") as f:
            f.write("curve,tau_s,adev\n")
            for name, c in curves.items():
                for tau, v in c:
                    f.write("%s,%.6g,%.6g\n" % (name, tau, v))
        print("  curves written to", a.allan_csv)


if __name__ == "__main__":
    main()
