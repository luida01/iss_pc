#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Cobertura del stencil del asistente de motion (docs/analysis/motion-assist.md).

Mide que fraccion de la elipse del stencil queda pintada por el patron del asistente
(port/src/runtime_pad.c, motion_assist_at): el spray solo esta activo una parte de
cada ciclo de 0.5 s, y cada muestra pinta un disco de radio "brush" alrededor de la
mira (yaw, aim_pitch) de ese instante.

    python tools/motion_coverage.py
"""
import math

BRUSH = 0.10


def coverage(pattern, T, ellipse=(0.65, 0.55), brush=BRUSH, step=0.05, spray_from=0.20):
    A, B = ellipse
    cells = []
    y = -A
    while y <= A + 1e-9:
        p = -B
        while p <= B + 1e-9:
            if (y * y) / (A * A) + (p * p) / (B * B) <= 1.0:
                cells.append([y, p, False])
            p += step
        y += step
    if not cells:
        return 0.0
    hit = 0
    t = 0.0
    while t < T:
        if (t % 0.5) >= spray_from:
            yaw, pitch = pattern(t)
            for c in cells:
                if not c[2]:
                    dy = yaw - c[0]
                    dp = pitch - c[1]
                    if dy * dy + dp * dp <= brush * brush:
                        c[2] = True
                        hit += 1
        t += 1.0 / 60.0
    return hit / len(cells)


def max_speed(pattern, T=20.0):
    """Velocidad angular maxima del trazo (grados/s): una pasada muy rapida deja huecos."""
    best = 0.0
    t = 0.0
    prev = pattern(t)
    while t < T:
        t += 1.0 / 60.0
        cur = pattern(t)
        if (t % 0.5) >= 0.20:
            sp = math.hypot(cur[0] - prev[0], cur[1] - prev[1]) * 60.0 * 180.0 / math.pi
            best = max(best, sp)
        prev = cur
    return best


def spiral(freq=0.45, breath=8.0, r0=0.2, r1=0.7, pitch_k=0.85):
    def f(t):
        a = 2 * math.pi * freq * t
        r = r0 + (r1 - r0) * (0.5 - 0.5 * math.cos(2 * math.pi * t / breath))
        return r * math.sin(a), pitch_k * r * math.cos(a)
    return f


def lissajous(fy, fp, phase=0.0, A=0.75, B=0.60):
    def f(t):
        return A * math.sin(2 * math.pi * fy * t), B * math.sin(2 * math.pi * fp * t + phase)
    return f


CANDIDATES = {
    'codigo anterior: espiral 0.30 Hz, r 0.2..0.7': spiral(freq=0.30, breath=10.0),
    'espiral 0.45 Hz, respiro 8 s': spiral(),
    'espiral 0.55 Hz, respiro 6 s': spiral(freq=0.55, breath=6.0),
    'lissajous 0.37/0.35 (elegido)': lissajous(0.37, 0.35),
    'lissajous 0.37/0.61': lissajous(0.37, 0.61),
    'lissajous 0.31/0.19': lissajous(0.31, 0.19),
    'lissajous 0.43/0.51': lissajous(0.43, 0.51),
}

ELLIPSES = {
    'stencil 0.65x0.55': (0.65, 0.55),
    'ancha 0.75x0.60': (0.75, 0.60),
    'amplia 0.90x0.75': (0.90, 0.75),
}


def main():
    print(f"{'patron':40s} {'vel':>7s}  " + '  '.join(f'{n:>18s}' for n in ELLIPSES))
    for name, pat in CANDIDATES.items():
        row = []
        for ellipse in ELLIPSES.values():
            row.append(f'{coverage(pat, 30, ellipse)*100:5.1f}%/{coverage(pat, 60, ellipse)*100:5.1f}%')
        print(f'{name:40s} {max_speed(pat):6.0f}d/s  ' + '  '.join(f'{r:>18s}' for r in row))
    print('\n(por cada elipse: cobertura a 30 s / a 60 s)')


if __name__ == '__main__':
    main()
