import { useEffect, useRef, useState } from 'react'
import { motion, MotionConfig, AnimatePresence, useReducedMotion, useMotionValue, useMotionValueEvent, useSpring, useTransform } from 'motion/react'
import { ClerkFailed, ClerkLoaded, ClerkLoading, Show, SignInButton, SignOutButton, useAuth, useUser } from '@clerk/react'
import { nav, hero, license, screens, features, start, requirements, download, account, cookies, notFound, footer } from './content'
import Mist, { fogFrames } from './Mist'

// no backdrop-blur: behind a card there is only the fog, which is already soft, and blurring it again cost a pass per card per frame
const glass = 'frost rounded-xl border border-white/[.07] shadow-[inset_0_1px_0_rgb(255_255_255/.05)]'
// small mono caps carry the navbar's magenta/cyan fringe; labels are also tilted like the nav labels
const mono = 'glitch-text font-mono text-[11px] uppercase tracking-[.14em] [--fringe:.3]'
const label = `${mono} -skew-x-6`
const button = `${mono} inline-block rounded-full px-6 py-3.5 transition-colors`
// the purple of the lit nav cell
const primary = `${button} bg-purple-800 text-white hover:bg-purple-700 disabled:opacity-60 aria-disabled:pointer-events-none aria-disabled:opacity-50`
const ghost = `${button} border border-white/20 text-white/85 hover:border-white/50 hover:text-white`
const rise = {
  initial: { opacity: 0, y: 24 },
  whileInView: { opacity: 1, y: 0 },
  viewport: { once: true, margin: '-60px' },
  transition: { duration: 0.8, ease: [0.2, 0.7, 0.2, 1] },
}

const cover = ([a, b, pattern]) => ({
  background: [
    pattern === 'rings' && 'repeating-radial-gradient(circle at 50% 50%, transparent 0 9px, rgb(255 255 255 / .07) 9px 10px)',
    pattern === 'lines' && 'repeating-linear-gradient(0deg, transparent 0 7px, rgb(255 255 255 / .08) 7px 8px)',
    `radial-gradient(circle at 25% 30%, ${a}, transparent 60%)`,
    `radial-gradient(circle at 80% 85%, ${b}, transparent 55%)`,
    '#0b0f15',
  ].filter(Boolean).join(','),
})

/* ---------- background: drifting fog (see Mist.jsx) ---------- */

const GRAIN = `url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='200' height='200'%3E%3Cfilter id='n'%3E%3CfeTurbulence type='fractalNoise' baseFrequency='.8' numOctaves='3' stitchTiles='stitch'/%3E%3C/filter%3E%3Crect width='100%25' height='100%25' filter='url(%23n)'/%3E%3C/svg%3E")`

function Sky() {
  return (
    <div aria-hidden className="fixed inset-0 -z-10 overflow-hidden">
      <Mist />
      {/* plain alpha, not mix-blend: a full-screen blend group was redone on every fog frame */}
      <div className="absolute inset-0 opacity-[.11]" style={{ backgroundImage: GRAIN, backgroundSize: '120px' }} />
    </div>
  )
}

/* ---------- bottom navbar ---------- */

// Drawn for this site: straight strokes only (| \\ /), square ends and mitred corners, for a sharp, aggressive mark.
const L = { fill: 'none', stroke: 'currentColor', strokeWidth: 2.5, strokeLinecap: 'square', strokeLinejoin: 'miter' }
const icons = {
  home: <path d="M7 33L20 7l13 26M20 16v17" {...L} />, // a peak with a spine
  features: <path d="M7 15V7h8M25 7h8v8M33 25v8h-8M15 33H7v-8M15 25l10-10" {...L} />, // four loose corners round a slash: what's inside
  start: <path d="M9 8v24M16 8l16 12-16 12" {...L} />, // | >
  requirements: <path d="M6 21l6 6M17 26L34 9M6 34h28" {...L} />, // a check broken in two, over a line
  download: <path d="M20 5v8M20 17v10M11 20l9 9 9-9M7 34h10M23 34h10" {...L} />, // a broken shaft dropping onto a split line
  account: <path d="M20 6l5 5-5 5-5-5zM8 34l12-13 12 13" {...L} />, // diamond head, wedge shoulders
}

// Obsidian inks: near-black with deep purples and one brighter violet glint
// (drawn a little smaller and softer than listed, so dark gaps open between them as they drift)
const inks = [
  { c: '#3b0764', x: -20, y: -50, s: 140, d: 16 },
  { c: '#2e1065', x: -25, y: 40, s: 130, d: 20 },
  { c: '#581c87', x: 20, y: -5, s: 140, d: 18 },
  { c: '#1e1b4b', x: 65, y: 35, s: 120, d: 22 },
  { c: '#4c1d95', x: 70, y: -45, s: 120, d: 21 },
  { c: '#6b21a8', x: 35, y: 50, s: 110, d: 14 },
  { c: '#8b5cf6', x: 30, y: -30, s: 70, d: 12 },
]

// The drifting purple fill behind the active nav cell
function Liquid() {
  return (
    <div className="absolute inset-0 overflow-hidden bg-[#0c0814]">
      <div className="absolute -inset-[25%] blur-[9px]">
        {inks.map((b, i) => {
          const sx = i % 2 ? 1 : -1
          const sy = i % 3 ? 1 : -1
          return (
            // the drift is the `ink` keyframes in index.css: off the main thread, where motion wrote 7 transforms every frame
            <div
              key={i}
              className="ink absolute aspect-square rounded-full"
              style={{ left: `${b.x}%`, top: `${b.y}%`, height: `${b.s * 0.85}%`, background: `radial-gradient(closest-side, ${b.c} 25%, transparent 85%)`, '--sx': sx, '--sy': sy, '--d': `${b.d}s` }}
            />
          )
        })}
      </div>
      {/* stronger, finer grain than the page's, so the lit cell reads as textured */}
      <div className="absolute inset-0 mix-blend-overlay" style={{ backgroundImage: GRAIN, backgroundSize: '120px' }} />
    </div>
  )
}

// One cell's share of the fill: the whole cell on its own section, gone a section away. While it hands
// over it is a soft half circle centred on the border it shares with the other cell, shrinking into that
// border while the other cell's half grows out of it: together, one circle passing across.
function Fill({ pos, index }) {
  const mask = useTransform(pos, v => {
    const d = v - index // -1..1 while handing over; > 0: the other cell is on the right
    const a = Math.max(0, 1 - Math.abs(d))
    const r = a * a * (3 - 2 * a) * 190
    // a wide, eased falloff so the half circle reads as a glow, not an edge. It covers the whole cell from
    // r = 150, i.e. once the cell is ~80% filled, so a lit cell resting a little off its section shows no arc
    return `radial-gradient(circle at ${d > 0 ? 100 : 0}% 50%, #000 ${r - 50}%, rgb(0 0 0 / .45) ${r - 25}%, transparent ${r}%)`
  })
  return (
    <motion.div aria-hidden className="absolute inset-0" style={{ maskImage: mask, WebkitMaskImage: mask }}>
      <Liquid />
    </motion.div>
  )
}

// Glitch treatment: the mark with a magenta and a cyan copy pulled apart (CSS in index.css). Not remounted when the
// cell lights up: a remount would replay the hover glitch on a clicked cell; adding `glitch-in` starts the glitch instead.
const layers = ['g-red', 'g-blue', 'g-main']
function Glyph({ id, active, quiet }) {
  return (
    <span aria-hidden className={`glitch relative h-5 w-5 md:h-9 md:w-9 ${active ? 'glitch-lit' : ''} ${active && !quiet ? 'glitch-in' : ''}`}>
      {layers.map(l => <svg key={l} viewBox="0 0 40 40" className={`absolute inset-0 h-full w-full ${l}`}>{icons[id]}</svg>)}
    </span>
  )
}

function NavItem({ item, active, quiet, onClick, children }) {
  // whether it lit up quietly is decided once, when it lights up, and held while it stays lit: a later render
  // after the nav click's scroll has settled must not start the glitch late
  const was = useRef(false)
  const lit = useRef(false)
  if (active !== was.current) { was.current = active; lit.current = active && quiet }
  quiet = lit.current
  return (
    <a
      href={`#${item.id}`}
      onClick={onClick}
      aria-current={active ? 'location' : undefined}
      className={`relative flex flex-col items-center justify-center gap-1 border-l border-white/[.08] transition-colors duration-300 first:border-l-0 focus-visible:outline-2 focus-visible:-outline-offset-4 focus-visible:outline-current ${
        active ? 'text-white' : 'text-white/65 hover:bg-white/[.05] hover:text-white'
      }`}
    >
      {children}
      <Glyph id={item.id} active={active} quiet={quiet} />
      <span className={`glitch-text relative max-w-full -skew-x-6 truncate px-0.5 font-mono text-[10px] tracking-[-.03em] uppercase sm:tracking-[.08em] md:tracking-[.14em] ${active ? 'glitch-text-on' : ''} ${active && !quiet ? 'glitch-text-in' : ''}`}>{item.label}</span>
    </a>
  )
}

// Where the fill is, in cells (0 = Home): it follows how much of the screen each section takes up.
// As the next section's top rises from 90% to 10% of the screen the fill hands over to its cell,
// halfway when that section fills the lower half.
function scrollPosition(els) {
  const start = innerHeight * 0.9
  const end = innerHeight * 0.1
  let i = 0
  while (i < els.length - 1 && els[i + 1].getBoundingClientRect().top <= end) i++
  const next = els[i + 1]
  if (!next) return i
  const t = Math.min(1, Math.max(0, (start - next.getBoundingClientRect().top) / (start - end)))
  return i + t * t * (3 - 2 * t)
}

// The navbar's background: the bottom strip of the page's fog, copied from the fog canvas on each new fog frame, so
// the bar looks see-through while the page scrolling underneath stays hidden. Same low resolution as the fog, so
// the browser stretches it the same way.
function FogStrip() {
  const ref = useRef(null)
  useEffect(() => {
    const draw = fog => {
      const cv = ref.current
      const sh = (fog.height * cv.clientHeight) / innerHeight // the fog rows behind the bar
      const h = Math.max(1, Math.round(sh))
      if (cv.width !== fog.width || cv.height !== h) { cv.width = fog.width; cv.height = h }
      cv.getContext('2d').drawImage(fog, 0, fog.height - sh, fog.width, sh, 0, 0, cv.width, h)
    }
    fogFrames.add(draw)
    return () => { fogFrames.delete(draw) }
  }, [])
  return (
    <>
      <canvas ref={ref} aria-hidden className="absolute inset-0 h-full w-full" />
      <div aria-hidden className="absolute inset-0 opacity-[.03]" style={{ backgroundImage: GRAIN }} />
    </>
  )
}

// Vybrant runs on Windows and Linux only, so phones get no Download section and no Download cell
// which kind of device this is, for the "not available" note (iPads on iPadOS say Macintosh, but have touch)
const ua = navigator.userAgent
const device = /iPhone|iPod/.test(ua) ? 'ios' : /iPad/.test(ua) || (/Macintosh/.test(ua) && navigator.maxTouchPoints > 1) ? 'ipad' : /Android/.test(ua) ? 'android' : 'other'
const phoneNav = nav.filter(n => n.id !== 'download')

// true while the screen is at least Tailwind's md wide (48rem)
function useWide() {
  const q = '(min-width: 48rem)'
  const [wide, setWide] = useState(() => matchMedia(q).matches)
  useEffect(() => {
    const m = matchMedia(q)
    const on = () => setWide(m.matches)
    m.addEventListener('change', on)
    return () => m.removeEventListener('change', on)
  }, [])
  return wide
}

function NavBar() {
  const items = useWide() ? nav : phoneNav
  const [active, setActive] = useState(0)
  const [live, setLive] = useState('0,0') // the one or two cells holding any fill right now
  const pos = useMotionValue(0)
  // a soft, slightly overdamped spring: the fill trails the scroll and eases in and out, never overshooting
  const smooth = useSpring(pos, { stiffness: 55, damping: 18, mass: 1 })
  useMotionValueEvent(smooth, 'change', v => setLive(`${Math.floor(v)},${Math.ceil(v)}`))
  // A nav click scrolls the page by itself: the cells it passes and lands on light up without glitching (the hover
  // before the click already did). Over once the scrolling has been still for a moment.
  const jumping = useRef(false)
  const settle = useRef(0)
  const quietFor = ms => { clearTimeout(settle.current); settle.current = setTimeout(() => { jumping.current = false }, ms) }
  const onNavClick = () => { jumping.current = true; quietFor(300) }

  useEffect(() => {
    const els = items.map(n => document.getElementById(n.id))
    let raf = 0
    const update = () => {
      raf = 0
      const p = scrollPosition(els)
      pos.set(p)
      setActive(Math.round(p))
    }
    const onScroll = () => {
      if (jumping.current) quietFor(150)
      if (!raf) raf = requestAnimationFrame(update)
    }
    update()
    smooth.jump(pos.get()) // a page opened mid-scroll starts on its cell instead of sweeping there
    addEventListener('scroll', onScroll, { passive: true })
    addEventListener('resize', onScroll)
    return () => { cancelAnimationFrame(raf); removeEventListener('scroll', onScroll); removeEventListener('resize', onScroll) }
  }, [items, pos, smooth])

  const [a, b] = live.split(',').map(Number)
  return (
    // the fog strip, not backdrop-blur: the blur cost ~40% of the frame rate over the moving fog. The page colour
    // underneath only shows until the first fog frame.
    <nav aria-label="Main" className="fixed inset-x-0 bottom-0 z-20 overflow-hidden rounded-t-xl border-t border-white/[.08] bg-[#0b0a10]">
      <FogStrip />
      <div className="relative grid h-16 grid-cols-5 md:h-20 md:grid-cols-6">
        {items.map((n, i) => (
          <NavItem key={n.id} item={n} active={active === i} quiet={jumping.current} onClick={onNavClick}>
            {(i === a || i === b) && <Fill pos={smooth} index={i} />}
          </NavItem>
        ))}
      </div>
    </nav>
  )
}

/* ---------- pieces ---------- */


// box: glass card (the bento); without it the tile is bare media + text, like the rest of the page
function Tile({ item, className = '', ratio = 'aspect-square', box = true }) {
  return (
    <motion.a href={item.href} {...rise} whileHover={{ y: -4 }} className={`group flex flex-col transition-colors ${box ? `${glass} p-3 hover:border-white/15` : ''} ${className}`}>
      <div className={`relative overflow-hidden rounded-lg bg-[#0c0814] ${ratio}`} style={item.art && cover(item.art)}>
        {item.img && <img src={item.img} alt={item.alt ?? ''} loading="lazy" className="absolute inset-0 h-full w-full object-cover object-top" />}
        {item.mark && <span className={`${mono} absolute inset-0 grid place-items-center text-white/70`}>{item.mark}</span>}
        <div className="absolute inset-0 bg-white/0 transition-colors duration-500 group-hover:bg-white/[.04]" />
      </div>
      {item.tag && <p className={`${label} mt-3 px-1 text-white/45`}>{item.tag}</p>}
      <h3 className="mt-3 px-1 text-[15px] leading-snug text-white/90 md:text-base">{item.title}</h3>
      {item.text && <p className="mt-2 px-1 pb-1 text-sm leading-relaxed text-white/55">{item.text}</p>}
    </motion.a>
  )
}

function Heading({ eyebrow, title, body }) {
  return (
    <motion.div {...rise} className="mb-12 grid gap-6 md:mb-16 md:grid-cols-2 md:items-end">
      <div>
        <p className={`${label} text-white/40`}>{eyebrow}</p>
        <h2 className="mt-4 text-4xl font-light tracking-tight md:text-6xl">{title}</h2>
      </div>
      {body && <p className="max-w-md leading-relaxed text-white/55 md:justify-self-end">{body}</p>}
    </motion.div>
  )
}

// the one call-to-action card in the bento, like the reference's audit card
function LicenseCard() {
  return (
    <motion.div {...rise} className={`${glass} flex flex-col p-6 md:p-7`}>
      <img src={license.shot.img} alt={license.shot.alt} width="920" height="400" loading="lazy" className="mb-8 w-full rounded-lg border border-white/10 shadow-[0_24px_60px_-24px_rgb(124_58_237/.45)] lg:my-auto" />
      <h2 className="text-[28px] leading-tight tracking-tight md:text-3xl">{license.title}</h2>
      <p className="mt-4 leading-relaxed text-white/60">{license.body}</p>
      <a href={license.cta.href} className={`${primary} mt-8 self-start`}>{license.cta.label} →</a>
    </motion.div>
  )
}

const section = 'mt-40 md:mt-56' // margin, not padding: a nav link lands on the heading, not on the gap above it

/* ---------- hero ---------- */


// big two-line words, bottom left, swapping every few seconds (held still with reduced motion)
function Words() {
  const reduce = useReducedMotion()
  const [i, setI] = useState(0)
  useEffect(() => {
    if (reduce) return
    const id = setInterval(() => setI(n => (n + 1) % hero.pairs.length), 3200)
    return () => clearInterval(id)
  }, [reduce])
  const [a, b] = hero.pairs[i]
  return (
    <h1 className="relative">
      <span className="sr-only">{hero.label}: {hero.tagline.join(' ')}</span>
      <AnimatePresence mode="popLayout" initial={false}>
        <motion.span
          key={i}
          aria-hidden
          className="block"
          initial={{ opacity: 0, y: 40, filter: 'blur(10px)' }}
          animate={{ opacity: 1, y: 0, filter: 'blur(0px)' }}
          exit={{ opacity: 0, y: -30, filter: 'blur(10px)' }}
          transition={{ duration: 0.7, ease: [0.2, 0.7, 0.2, 1] }}
        >
          <span className="block text-5xl font-light tracking-tight text-white/80 md:text-6xl xl:text-7xl">{a}</span>
          <span className="block text-[17vw] leading-[.85] tracking-tighter md:text-[9rem] xl:text-[11rem]">{b}</span>
        </motion.span>
      </AnimatePresence>
    </h1>
  )
}

function Hero() {
  return (
    // clip: on phones the ring is wider than the screen
    <div className="relative flex min-h-[calc(100svh-4rem)] flex-col justify-between gap-10 overflow-x-clip py-8 md:min-h-[calc(100svh-5rem)] md:py-10">

      <motion.div {...rise} className="flex flex-col gap-8 md:flex-row md:items-start md:justify-between">
        <img src="/logo.png" alt={hero.label} width="44" height="44" className="h-11 w-11" />
        <div className="flex flex-col items-start md:items-end md:text-right">
          <p className={`${label} text-white/60`}>{hero.tagline.map(l => <span key={l} className="block">{l}</span>)}</p>
          <p className="mt-1 text-6xl font-light tracking-tighter md:text-7xl">{hero.word}</p>
          <a href={hero.cta.href} className={`${ghost} mt-5 max-md:hidden`}>{hero.cta.label}</a>
        </div>
      </motion.div>

      {/* the line through the middle, stopping short of the app, which sits inside a broken ring */}
      <motion.div {...rise} className="flex items-center justify-center gap-6 md:gap-8">
        <span aria-hidden className="hidden h-px flex-1 bg-gradient-to-r from-white/15 from-60% to-transparent md:block" />
        <div className="relative w-full max-w-[min(34rem,calc((100svh_-_4rem)*0.67))] shrink-0">
          {/* sized from the screenshot: a 3:1 picture's corners fit inside from 106% of its width; the picture is capped by the
              screen height so the ring stays within about four fifths of the hero */}
          <svg aria-hidden viewBox="0 0 100 100" className="pointer-events-none absolute top-1/2 left-1/2 -z-10 aspect-square w-[122%] -translate-x-1/2 -translate-y-1/2">
            {/* two quarter arcs, over the top left (210° to 300°) and under the bottom right (30° to 120°); each fades out at both
                ends through a gradient laid along its chord */}
            <defs>
              {[['arc-a', 7.13, 25.25, 74.75, 7.13], ['arc-b', 92.87, 74.75, 25.25, 92.87]].map(([id, x1, y1, x2, y2]) => (
                <linearGradient key={id} id={id} gradientUnits="userSpaceOnUse" x1={x1} y1={y1} x2={x2} y2={y2}>
                  <stop offset="0" stopColor="#fff" stopOpacity="0" />
                  <stop offset=".15" stopColor="#fff" stopOpacity=".14" />
                  <stop offset=".85" stopColor="#fff" stopOpacity=".14" />
                  <stop offset="1" stopColor="#fff" stopOpacity="0" />
                </linearGradient>
              ))}
            </defs>
            <path d="M7.13 25.25A49.5 49.5 0 0 1 74.75 7.13" fill="none" stroke="url(#arc-a)" strokeWidth="1" vectorEffect="non-scaling-stroke" />
            <path d="M92.87 74.75A49.5 49.5 0 0 1 25.25 92.87" fill="none" stroke="url(#arc-b)" strokeWidth="1" vectorEffect="non-scaling-stroke" />
          </svg>
          <a href="#features" className="group relative block">
            <span className="block aspect-[3/1] overflow-hidden rounded-lg">
              <img src={hero.shot.img} alt={hero.shot.alt} className="h-full w-full object-cover object-top transition-[filter,transform] duration-700 group-hover:scale-[1.03] group-hover:brightness-110" />
            </span>
          </a>
        </div>
        <span aria-hidden className="hidden h-px flex-1 bg-gradient-to-l from-white/15 from-60% to-transparent md:block" />
      </motion.div>

      <motion.div {...rise}><Words /></motion.div>
    </div>
  )
}

/* ---------- account: Vybrant's license API ---------- */

const API = import.meta.env.VITE_API_URL ?? ''
// where latest.json is read from: in development through Vite's /downloads proxy (the feed only allows the live site)
const FEED = import.meta.env.VITE_DOWNLOADS_URL || (import.meta.env.DEV ? '/downloads' : 'https://download.vybrant.studio')
const FILES = 'https://download.vybrant.studio' // where the files themselves are

// The latest release from the feed Vybrant's updater uses: file links per system; null while loading, false if unreachable
function useLatestRelease() {
  const [release, setRelease] = useState(null)
  useEffect(() => {
    fetch(`${FEED}/latest.json`)
      .then(r => (r.ok ? r.json() : Promise.reject()))
      .then(r => setRelease({ version: r.version, linux: r.linux && `${FILES}/${r.linux.path}`, windows: r.windows && `${FILES}/${r.windows.path}` }))
      .catch(() => setRelease(false))
  }, [])
  return release
}

// The site only sets the cookies sign-in needs, which need no consent; this says so, once
function CookieNotice() {
  const [open, setOpen] = useState(() => { try { return localStorage.getItem('cookie-notice-seen') !== '1' } catch { return true } })
  if (!open) return null
  const close = () => {
    try { localStorage.setItem('cookie-notice-seen', '1') } catch { /* it just comes back next visit */ }
    setOpen(false)
  }
  return (
    <div role="region" aria-label="Cookies" className={`${glass} fixed right-4 bottom-20 left-4 z-30 flex items-center gap-4 bg-[#0c0a12]/90 p-4 sm:left-auto sm:max-w-sm md:bottom-24`}>
      <p className="text-sm leading-relaxed text-white/70">{cookies.text}</p>
      <button className={`${mono} shrink-0 rounded-full bg-white/90 px-4 py-2.5 text-[#060509] hover:bg-white`} onClick={close}>{cookies.ok}</button>
    </div>
  )
}

// GET: the account's license (null before one was made); POST: make it (the server returns the existing one if there is one)
async function licenseCall(method, token) {
  const res = await fetch(`${API}/api/license`, { method, headers: { Authorization: `Bearer ${token}` } })
  if (!res.ok) throw new Error(`The license server answered ${res.status}. Try again in a moment.`)
  return (await res.json()).license
}

function LicensePanel() {
  const { getToken } = useAuth()
  const { user } = useUser()
  const [key, setKey] = useState(undefined) // undefined: still asking; null: none made yet
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const [copied, setCopied] = useState(false)

  const run = async method => {
    setBusy(true)
    setError('')
    try {
      setKey(await licenseCall(method, await getToken()))
    } catch (e) {
      setError(e.message)
    } finally {
      setBusy(false)
    }
  }
  useEffect(() => { run('GET') }, [])

  const copy = async () => {
    await navigator.clipboard.writeText(key)
    setCopied(true)
    setTimeout(() => setCopied(false), 1800)
  }

  return (
    <div>
      <div className="flex flex-wrap items-center justify-between gap-4">
        <p className={`${mono} text-white/50`}>{user?.primaryEmailAddress?.emailAddress}</p>
        <SignOutButton><button className={ghost}>{account.signOut}</button></SignOutButton>
      </div>
      {error && <p role="alert" className="mt-6 text-red-300">{error}</p>}
      {key === undefined && !error && <p className="mt-6 text-white/50">{account.looking}</p>}
      {key === null && (
        <div className="mt-6">
          <p className="max-w-md leading-relaxed text-white/60">{account.body}</p>
          <button className={`${primary} mt-6`} onClick={() => run('POST')} disabled={busy}>{busy ? account.generating : account.generate}</button>
        </div>
      )}
      {key && (
        <motion.div initial={{ opacity: 0, filter: 'blur(8px)' }} animate={{ opacity: 1, filter: 'blur(0px)' }} transition={{ duration: 0.6 }}>
          <pre aria-label="License key" className="mt-6 overflow-x-auto rounded-lg border border-white/10 bg-black/30 p-4 font-mono text-xs leading-relaxed break-all whitespace-pre-wrap text-white/80">{key}</pre>
          <div className="mt-6 flex flex-wrap gap-3" aria-live="polite">
            <button className={primary} onClick={copy}>{copied ? account.copied : account.copy}</button>
            <a href={account.download.href} className={`${ghost} max-md:hidden`}>{account.download.label}</a>
          </div>
          <p className="mt-4 text-sm text-white/45">{account.hint}</p>
        </motion.div>
      )}
    </div>
  )
}

function Account() {
  return (
    // at least a screen tall: the last section has to reach the top of the screen for its cell to fully light
    <section id="account" className={`${section} flex min-h-[calc(100svh-4rem)] flex-col md:min-h-[calc(100svh-5rem)]`}>
      <Heading eyebrow={account.eyebrow} title={account.title} body={account.body} />
      <motion.div {...rise} className={`${glass} mb-40 p-6 md:p-10`}>
        <ClerkLoading><p className="text-white/50">{account.loading}</p></ClerkLoading>
        {/* the sign-in service only answers on the live site's own domain */}
        <ClerkFailed>
          <p role="alert" className="text-white/60">{account.unavailable}</p>
        </ClerkFailed>
        <ClerkLoaded>
          <Show when="signed-out">
            <p className="max-w-md leading-relaxed text-white/60">{account.signedOut}</p>
            <SignInButton mode="modal"><button className={`${primary} mt-6`}>{account.signIn}</button></SignInButton>
          </Show>
          <Show when="signed-in"><LicensePanel /></Show>
        </ClerkLoaded>
      </motion.div>

      {/* inside the last section so the nav marks it active at the bottom of the page */}
      <footer className="mt-auto flex flex-col items-start justify-between gap-4 border-t border-white/[.07] pt-8 md:flex-row md:items-end">
        <p className="text-[18vw] leading-[.8] font-light tracking-tighter text-transparent [text-shadow:-.025em_0_rgb(224_64_251/.16),.025em_0_rgb(34_211_238/.16)] md:text-[11vw]">{footer.brand}</p>
      </footer>
    </section>
  )
}

/* ---------- page ---------- */

function NotFound() {
  useEffect(() => { document.title = notFound.title }, [])
  return (
    <MotionConfig reducedMotion="user">
      <Sky />
      {/* overflow-y-clip: the big 404 hangs below its tight line box and would make this one-screen page scroll */}
      <main className="mx-auto flex min-h-svh max-w-[1400px] flex-col justify-between gap-16 overflow-y-clip px-5 py-8 md:px-10 md:py-10 lg:px-16">
        <a href="/" aria-label={notFound.cta} className="self-start"><img src="/logo.png" alt="" width="44" height="44" className="h-11 w-11" /></a>
        <motion.div {...rise} className="grid gap-10 md:grid-cols-2 md:items-end">
          <p aria-hidden className="glitch-loop text-[38vw] leading-[.8] font-light tracking-tighter text-white/90 [--g:.006em] md:text-[16rem]">{notFound.code}</p>
          <div>
            <h1 className="text-4xl font-light tracking-tight md:text-6xl">{notFound.heading}</h1>
            <p className="glitch-loop mt-6 max-w-md leading-relaxed text-white/55 [animation-delay:.25s]">{notFound.body}</p>
            <a href="/" className={`${primary} mt-8`}>{notFound.cta} →</a>
          </div>
        </motion.div>
      </main>
    </MotionConfig>
  )
}

// the Vybrant app and old links open /account and /download: on this one page they are sections
const OLD = { '/account': 'account', '/download': 'download', '/changelog': 'download' }
const path = location.pathname.replace(/\/$/, '') || '/'

export default function App() {
  return path === '/' || path === '/index.html' || path in OLD ? <Site /> : <NotFound />
}

function Site() {
  const release = useLatestRelease()
  const wide = useWide()
  useEffect(() => {
    const old = OLD[path]
    if (old) history.replaceState(null, '', `/#${old}`)
    if (location.hash) document.getElementById(location.hash.slice(1))?.scrollIntoView({ behavior: 'instant' }) // the page wasn't there yet when the browser looked
  }, [])

  return (
    <MotionConfig reducedMotion="user">
      <Sky />
      <main className="mx-auto max-w-[1400px] px-5 pb-40 md:px-10 lg:px-16">
        <section id="home">
          <Hero />

          {/* the one boxed area, like the reference's content hub */}
          <div className="grid gap-4 pt-24 lg:grid-cols-[1fr_2fr]">
            <LicenseCard />
            {/* two wide screenshots over three small ones; on phones the last small one runs full width */}
            <div className="grid grid-cols-2 gap-4 md:grid-cols-6">
              {screens.map((t, i) => {
                const last = i === screens.length - 1
                return (
                  <Tile
                    key={t.tag}
                    item={t}
                    className={t.wide ? 'col-span-2 md:col-span-3' : last ? 'col-span-2' : 'md:col-span-2'}
                    ratio={t.wide ? 'aspect-[2/1]' : last ? 'aspect-[2/1] md:aspect-square' : 'aspect-square'}
                  />
                )
              })}
            </div>
          </div>
        </section>

        <section id="features" className={section}>
          <Heading {...features} />
          <div className="grid gap-x-6 gap-y-14 sm:grid-cols-2 lg:grid-cols-4">
            {features.items.map(f => <Tile key={f.title} item={f} ratio="aspect-[16/10]" box={false} />)}
          </div>
        </section>

        <section id="start" className={section}>
          <Heading {...start} />
          <ol className="grid gap-10 md:grid-cols-3">
            {start.steps.map((st, i) => (
              <motion.li key={st.title} {...rise} className="border-t border-white/[.08] pt-6">
                <span className="glitch-text block -skew-x-6 font-mono text-6xl font-light tracking-tighter text-white/20 [--fringe:.25]">{String(i + 1).padStart(2, '0')}</span>
                <h3 className="mt-6 text-xl">{st.desktopOnly && !wide ? <span aria-disabled="true" className="text-white/30">{st.title}<span className={`${label} mt-2 block text-white/45`}>{start.unavailable[device]}</span></span> : st.href ? <a href={st.href} className="underline decoration-white/25 underline-offset-4 hover:decoration-white">{st.title}</a> : st.title}</h3>
                <p className="mt-3 max-w-sm leading-relaxed text-white/55">{st.text}</p>
              </motion.li>
            ))}
          </ol>
        </section>

        <section id="requirements" className={section}>
          <Heading {...requirements} />
          <motion.div {...rise}>
            <div className={`${mono} hidden gap-6 border-b border-white/10 pb-4 text-white/35 md:grid md:grid-cols-[200px_1fr_1fr]`}>
              {requirements.columns.map(c => <span key={c}>{c}</span>)}
            </div>
            <ul className="divide-y divide-white/[.08] border-b border-white/[.08]">
              {requirements.rows.map(([k, ...vals]) => (
                <li key={k} className="grid gap-3 py-6 md:grid-cols-[200px_1fr_1fr] md:gap-6">
                  <span className={`${label} self-start text-white/60`}>{k}</span>
                  {vals.map((v, i) => (
                    <span key={i} className="leading-relaxed text-white/75">
                      <span className={`${mono} mr-2 text-white/30 md:hidden`}>{requirements.columns[i + 1]}</span>{v}
                    </span>
                  ))}
                </li>
              ))}
            </ul>
          </motion.div>
        </section>

        <section id="download" className={`${section} max-md:hidden`}>
          <Heading {...download} />
          <div className="grid gap-10 md:grid-cols-3">
            {download.builds.map(d => (
              <motion.div key={d.name} {...rise} className="border-t border-white/[.08] pt-6">
                <p className="text-3xl font-light tracking-tight">{d.name}</p>
                <p className={`${label} mt-2 text-white/40`}>{d.kind}</p>
                <a href={release?.[d.os]} aria-disabled={!release?.[d.os]} className={`${primary} mt-6`}>Download for {d.name} →</a>
              </motion.div>
            ))}
            <motion.div {...rise} className="border-t border-white/[.08] pt-6">
              <p className="text-3xl font-light tracking-tight">License</p>
              <p className={`${label} mt-2 text-white/40`}>{download.license.note}</p>
              <a href={download.license.href} className={`${ghost} mt-6`}>{download.license.label}</a>
            </motion.div>
          </div>
          {release === false && <p role="status" className="mt-8 text-white/50">{download.unavailable}</p>}
        </section>

        <Account />
      </main>
      <NavBar />
      <CookieNotice />
    </MotionConfig>
  )
}
