# Run: python3 resources/make_icon.py > resources/vybrant.svg && cp resources/vybrant.svg web/public/logo.svg
# Vybrant icon: a geometric V with a ribbon of waveform lines across it; the lines turn navy where they cross the V.
# No clipPath (Qt's SVG renderer ignores it): each line is cut into inside / outside pieces here.
import math, sys
S = 0.165                                   # reference image units -> icon units
def p(x, y): return (128 + (x - 980) * S, 128 - 885 * S / 2 + (y - 60) * S)
left = [p(550, 60), p(765, 60), p(1090, 945), p(875, 945)]
right = [p(1195, 60), p(1410, 60), p(1090, 945), p(875, 945)]
def inside_poly(pt, poly):
    x, y = pt; c = False
    for i in range(len(poly)):
        (x1, y1), (x2, y2) = poly[i], poly[i - 1]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1: c = not c
    return c
def in_v(pt): return inside_poly(pt, left) or inside_poly(pt, right)
# the slit: the right stroke's inner edge where it runs over the left stroke
def isect(a, b, c, d):
    (x1, y1), (x2, y2), (x3, y3), (x4, y4) = a, b, c, d
    t = ((x1 - x3) * (y3 - y4) - (y1 - y3) * (x3 - x4)) / ((x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4))
    return (x1 + t * (x2 - x1), y1 + t * (y2 - y1))
slit_top = isect(right[0], right[3], left[1], left[2])
axis = p(0, 625)[1]
lam, sigma, amp, centre = 54.0, 50.0, 60.0, p(1010, 0)[0]
peak = p(950, 0)[0]
x0 = peak - lam / 4
N = 15
white, navy = [], []
for i in range(N):
    phi, a = i * 0.17, 1 - i * 0.04
    pts = []
    x = 22.0
    while x <= 234.0:
        g = math.exp(-((x - centre) ** 2) / (2 * sigma ** 2)) * 0.85 + 0.15 * math.exp(-((x - centre) ** 2) / (2 * (sigma * 2.2) ** 2))
        fade = min(1, (x - 22) / 26, (234 - x) / 26)        # flat ends, like the reference
        y = axis - amp * a * g * fade * math.sin(2 * math.pi * (x - x0) / lam + phi)
        pts.append((x, y)); x += 1.0
    seg, flag = [pts[0]], in_v(pts[0])
    for q in pts[1:]:
        f = in_v(q)
        if f != flag:
            (navy if flag else white).append(seg); seg = [seg[-1]]; flag = f
        seg.append(q)
    (navy if flag else white).append(seg)
def d(segs): return " ".join("M" + " L".join(f"{x:.1f} {y:.1f}" for x, y in s) for s in segs if len(s) > 1)
poly = lambda pp: " ".join(f"{x:.1f},{y:.1f}" for x, y in pp)
print(f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">
  <!-- Vybrant: a geometric V with a ribbon of waveform lines across it (navy where they cross the V) -->
  <rect width="256" height="256" rx="56" fill="#000"/>
  <rect x="1.5" y="1.5" width="253" height="253" rx="54.5" fill="none" stroke="#fff" stroke-opacity="0.12" stroke-width="3"/>
  <polygon points="{poly(left)}" fill="#fff"/>
  <polygon points="{poly(right)}" fill="#fff"/>
  <line x1="{slit_top[0]:.1f}" y1="{slit_top[1]:.1f}" x2="{right[3][0]:.1f}" y2="{right[3][1]:.1f}" stroke="#000" stroke-width="2"/>
  <path d="{d(white)}" fill="none" stroke="#fff" stroke-width="1" stroke-linecap="round" stroke-linejoin="round"/>
  <path d="{d(navy)}" fill="none" stroke="#1c1e3c" stroke-width="1" stroke-linecap="round" stroke-linejoin="round"/>
</svg>''')
