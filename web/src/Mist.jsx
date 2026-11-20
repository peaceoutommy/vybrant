import { useEffect, useRef } from 'react'

// Drifting cloud background, drawn on the CPU (no WebGL): a small canvas is filled pixel by pixel
// with domain-warped noise and the browser stretches it to full screen. Clouds are soft, so ~200px wide is enough.

const N = 256 // noise tile size (power of two, so wrapping is a bitmask)
const smooth = (a, b, x) => { const t = Math.min(1, Math.max(0, (x - a) / (b - a))); return t * t * (3 - 2 * t) }

// Tileable fractal value noise, 0..1, computed once
function makeNoise() {
  const out = new Float32Array(N * N)
  let seed = 7, amp = 0.5, total = 0
  const rand = () => (seed = (seed * 16807) % 2147483647) / 2147483647
  for (const period of [4, 8, 16, 32, 64]) {
    const lat = Float32Array.from({ length: period * period }, rand)
    for (let y = 0; y < N; y++) {
      const fy = (y / N) * period, y0 = Math.floor(fy), ty = smooth(0, 1, fy - y0), y1 = (y0 + 1) % period
      for (let x = 0; x < N; x++) {
        const fx = (x / N) * period, x0 = Math.floor(fx), tx = smooth(0, 1, fx - x0), x1 = (x0 + 1) % period
        const a = lat[y0 * period + x0], b = lat[y0 * period + x1], c = lat[y1 * period + x0], d = lat[y1 * period + x1]
        out[y * N + x] += amp * (a + (b - a) * tx + (c - a + (a - b - c + d) * tx) * ty)
      }
    }
    total += amp
    amp *= 0.5
  }
  for (let i = 0; i < out.length; i++) out[i] /= total
  return out
}

// Bilinear sample at (u, v) in tile units (1 = one tile), wrapping
function sampler(tex) {
  return (u, v) => {
    u *= N; v *= N
    const xf = Math.floor(u), yf = Math.floor(v), tx = u - xf, ty = v - yf
    const x0 = xf & (N - 1), x1 = (xf + 1) & (N - 1), y0 = (yf & (N - 1)) * N, y1 = ((yf + 1) & (N - 1)) * N
    const a = tex[y0 + x0], b = tex[y0 + x1], c = tex[y1 + x0], d = tex[y1 + x1]
    return a + (b - a) * tx + (c - a + (a - b - c + d) * tx) * ty
  }
}

// obsidian: near-black with dark purple fog
const BASE = [11, 10, 16] // page background #0b0a10
const PURPLE = [70, 34, 120]
const INDIGO = [40, 28, 96]
const PLUM = [98, 34, 112]

// Other places that show the fog (the navbar) listen here: each is called with the canvas after every new frame
export const fogFrames = new Set()

export default function Mist() {
  const ref = useRef(null)

  useEffect(() => {
    const cv = ref.current
    const ctx = cv.getContext('2d')
    const T = sampler(makeNoise())
    console.assert(Math.abs(T(0, 0.3) - T(1, 0.3)) < 1e-6, 'mist noise should tile')
    const still = matchMedia('(prefers-reduced-motion: reduce)').matches
    let W, H, img, glow, raf, last = 0, lastY = -1

    const size = () => {
      W = 160
      H = Math.max(1, Math.round((W * innerHeight) / innerWidth))
      cv.width = W
      cv.height = H
      img = ctx.createImageData(W, H)
      // brighter toward the middle, like a light inside the fog (fixed, so worked out once)
      glow = Float32Array.from({ length: W * H }, (_, i) => {
        const cx = (i % W - W / 2) / H, cy = Math.floor(i / W) / H - 0.5
        // the vignette, folded in: it was a full-screen gradient layer (transparent at 35% of the way to the corner, page colour at the corner)
        const vx = (i % W) / W - 0.5, vy = Math.floor(i / W) / H - 0.5
        const edge = Math.min(1, Math.max(0, (Math.sqrt(2 * (vx * vx + vy * vy)) - 0.35) / 0.65))
        return (0.3 + 0.9 * Math.exp(-(cx * cx + cy * cy) * 3)) * 0.8 * (1 - 0.7 * edge)
      })
    }

    const draw = ms => {
      raf = still ? 0 : requestAnimationFrame(draw)
      // every new fog frame makes the browser redo everything drawn over it, so: ~24fps while the page scrolls (the fog
      // slides with it), ~12fps at rest, where it drifts well under a pixel per frame
      if (ms - last < (scrollY === lastY ? 83 : 42)) return
      last = ms
      lastY = scrollY
      const t = ms / 1000 * 0.012
      const scroll = scrollY / innerHeight * 0.12 // the fog slides a little as the page scrolls
      const px = img.data
      for (let y = 0, i = 0; y < H; y++) {
        const sy = y / H + scroll
        for (let x = 0; x < W; x++, i += 4) {
          const sx = x / H
          // warp field: two slow noise lookups bend the cloud so it curls instead of just sliding
          const qx = T(sx * 0.8 + t, sy * 0.8)
          const qy = T(sx * 0.8 + 0.37, sy * 0.8 - t * 0.8 + 0.61)
          const n = T(sx * 1.5 + qx * 0.6 - t * 0.5, sy * 1.5 + qy * 0.6)
          let d = smooth(0.38, 0.8, n)
          // big soft patches of shade wander across, leaving dark gaps between the clouds
          d *= 0.2 + 0.8 * smooth(0.32, 0.68, T(sx * 0.22 - t * 0.6 + 0.13, sy * 0.22 + t * 0.4 + 0.71))
          d *= glow[i >> 2]
          // colour: purple drifting to indigo, and plum where that noise peaks
          const hn = T(sx * 0.3 + 0.51, sy * 0.3 + t * 0.7)
          const h = smooth(0.35, 0.65, hn)
          const v = smooth(0.66, 0.8, hn) * 0.6
          for (let k = 0; k < 3; k++) {
            const c = PURPLE[k] + (INDIGO[k] - PURPLE[k]) * h
            px[i + k] = BASE[k] + (c + (PLUM[k] - c) * v) * d
          }
          px[i + 3] = 255
        }
      }
      ctx.putImageData(img, 0, 0)
      fogFrames.forEach(f => f(cv))
    }

    size()
    const onResize = () => { size(); if (still) draw(performance.now()) }
    addEventListener('resize', onResize)
    raf = requestAnimationFrame(draw)
    return () => { cancelAnimationFrame(raf); removeEventListener('resize', onResize) }
  }, [])

  return <canvas ref={ref} aria-hidden className="absolute inset-0 h-full w-full" />
}
