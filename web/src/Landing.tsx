import { useEffect, useRef, useState } from 'react';
import { AnimatePresence, animate, motion, useMotionValue, useScroll, useTransform } from 'motion/react';
import { Footer, Nav } from './Nav';
import { buildOs, osName } from './platform';
import { useLatestRelease } from './release';

const API = import.meta.env.VITE_API_URL ?? '';

// How many licenses have been handed out (the license API counts them): null until known, and if it can't be had
function useUserCount() {
  const [users, setUsers] = useState<number | null>(null);
  useEffect(() => {
    fetch(`${API}/api/stats`)
      .then((r) => (r.ok ? r.json() : Promise.reject()))
      .then((s: { licenses: number }) => setUsers(s.licenses))
      .catch(() => {});
  }, []);
  return users;
}

const features = [
  { title: 'Patterns and playlist', text: 'Step sequencer, piano roll and a playlist that arranges patterns, audio and automation.' },
  { title: 'Recording', text: 'Loop recording with takes and comping, count-in and punch in/out.' },
  { title: 'Mixer', text: 'Inserts, sends and sidechains, track freeze and latency-compensated renders.' },
  { title: 'Your plugins', text: 'VST2, VST3, CLAP and LV2. Each can run in its own process, so a crash stays contained.' },
  { title: 'Built-in sound', text: 'Samplers, synths and drums, plus EQ, dynamics, delay, reverb and modulation effects.' },
  { title: 'Export', text: 'Mixdowns, stems and MIDI files, at any sample rate.' },
];

const steps = [
  { title: 'Download Vybrant', text: 'For 64-bit Linux and Windows, the same file for every update.', href: '/download' },
  { title: 'Get your license', text: 'Sign in with Google or your email; one free license per account.', href: '/account' },
  { title: 'Paste it into Vybrant', text: 'Vybrant checks it on your machine. No connection needed after that.' },
];

// The app as it looks, one view after another (web/scripts/showcase.py makes these from a generated song)
const slides = [
  { src: '/showcase-pattern.webp', alt: 'A pattern in the channel rack and piano roll: drums, bass, chords and a lead melody' },
  { src: '/showcase-playlist.webp', alt: 'The playlist: patterns, an audio loop and automation clips arranged over 24 bars' },
  { src: '/showcase-mixer.webp', alt: 'The mixer: inserts with effects, a drum bus and a reverb return' },
  { src: '/showcase-plugins.webp', alt: 'Built-in effects: parametric EQ, compressor, reverb and delay' },
  { src: '/showcase-instruments.webp', alt: 'Built-in instruments: wavetable, subtractive and FM synths' },
];
// The next slide comes in from the side it is on and the last one leaves the other way, through a short soft focus
const slideMotion = {
  enter: (dir: number) => ({ opacity: 0, x: `${dir * 6}%`, scale: 1.02, filter: 'blur(8px)' }),
  center: { opacity: 1, x: '0%', scale: 1, filter: 'blur(0px)' },
  exit: (dir: number) => ({ opacity: 0, x: `${dir * -6}%`, scale: 0.98, filter: 'blur(8px)' }),
};

// Reveal as it scrolls into view, once
const reveal = {
  initial: { opacity: 0, y: 24 },
  whileInView: { opacity: 1, y: 0 },
  viewport: { once: true, margin: '-80px' },
  transition: { duration: 0.7, ease: [0.22, 1, 0.36, 1] },
} as const;

export function Landing() {
  const shot = useRef<HTMLDivElement>(null);
  const { scrollYProgress } = useScroll({ target: shot, offset: ['start end', 'center center'] });
  const tilt = useTransform(scrollYProgress, [0, 1], [14, 0]);   // the screenshot lies back, then stands up as it arrives
  const users = useUserCount();
  const counted = useMotionValue(0);
  const shownUsers = useTransform(counted, (v) => Math.round(v).toLocaleString('en'));   // counts up when it scrolls into view
  const [[slide, dir], setShown] = useState([0, 1]);   // dir: 1 = forwards, -1 = back
  const [paused, setPaused] = useState(false);   // while the pointer is on it
  const go = (to: number, d = to > slide ? 1 : -1) => setShown([(to + slides.length) % slides.length, d]);
  useEffect(() => {
    if (paused) return;
    const t = setTimeout(() => go(slide + 1, 1), 5000);
    return () => clearTimeout(t);
  }, [slide, paused]);
  useEffect(() => {
    for (const s of slides) new Image().src = s.src;   // decoded before their turn: no blank frame mid-transition
  }, []);
  const download = useLatestRelease().release?.[buildOs]?.url ?? '/download';   // the file itself; the download page until it's known

  return (
    <>
      <div className="glow" aria-hidden />
      <Nav />
      <main>
        <section className="hero container">
          <motion.p className="label" {...reveal}>
            Digital audio workstation · Linux · Windows
          </motion.p>
          <motion.h1 {...reveal} transition={{ ...reveal.transition, delay: 0.08 }}>
            From first loop
            <br />
            to <em>finished track.</em>
          </motion.h1>
          <motion.p className="lead" {...reveal} transition={{ ...reveal.transition, delay: 0.16 }}>
            Vybrant is a complete music studio for Linux and Windows. Sequence patterns, play parts into the piano roll, mix with sends and
            sidechains, and run your own plugins.
          </motion.p>
          <motion.div className="cta" {...reveal} transition={{ ...reveal.transition, delay: 0.24 }}>
            <a href={download} className="btn btn-primary">
              Download for {osName[buildOs]}
            </a>
            <a href="/account" className="btn btn-ghost">
              Get your free license
            </a>
          </motion.div>
          <motion.p className="caption" {...reveal} transition={{ ...reveal.transition, delay: 0.3 }}>
            Free · One license per account · Works offline
          </motion.p>
        </section>

        <div className="container shot-wrap" ref={shot}>
          <div className="carousel">
            <motion.div
              className="shot"
              style={{ rotateX: tilt }}
              initial={{ opacity: 0, y: 40 }}
              animate={{ opacity: 1, y: 0 }}
              transition={{ duration: 1, delay: 0.35, ease: [0.22, 1, 0.36, 1] }}
            >
              <div className="slides" onPointerEnter={() => setPaused(true)} onPointerLeave={() => setPaused(false)}>
                <AnimatePresence initial={false} custom={dir}>
                  <motion.img
                    key={slide}
                    src={slides[slide].src}
                    alt={slides[slide].alt}
                    width={2400}
                    height={1350}
                    custom={dir}
                    variants={slideMotion}
                    initial="enter"
                    animate="center"
                    exit="exit"
                    transition={{ duration: 0.9, ease: [0.22, 1, 0.36, 1] }}
                  />
                </AnimatePresence>
              </div>
            </motion.div>
            <button className="slide-arrow prev" aria-label="Previous screenshot" onClick={() => go(slide - 1, -1)}>
              <svg viewBox="0 0 24 24" aria-hidden><path d="M15 5l-7 7 7 7" /></svg>
            </button>
            <button className="slide-arrow next" aria-label="Next screenshot" onClick={() => go(slide + 1, 1)}>
              <svg viewBox="0 0 24 24" aria-hidden><path d="M9 5l7 7-7 7" /></svg>
            </button>
          </div>
          <div className="dots" role="group" aria-label="Screenshots">
            {slides.map((s, i) => (
              <button key={s.src} aria-label={s.alt} aria-current={i === slide} className={i === slide ? 'on' : undefined} onClick={() => go(i)} />
            ))}
          </div>
        </div>

        <section id="features" className="section container">
          <motion.p className="label" {...reveal}>
            What's inside
          </motion.p>
          <motion.h2 {...reveal}>Everything a track needs.</motion.h2>
          <ul className="features">
            {features.map((f, i) => (
              <motion.li key={f.title} {...reveal} transition={{ ...reveal.transition, delay: (i % 2) * 0.08 }}>
                <span className="index">{String(i + 1).padStart(2, '0')}</span>
                <div>
                  <h3>{f.title}</h3>
                  <p className="mono">{f.text}</p>
                </div>
              </motion.li>
            ))}
          </ul>
        </section>

        <section id="license" className="section container">
          <motion.p className="label" {...reveal}>
            Get started
          </motion.p>
          <motion.h2 {...reveal}>Three steps to your first track.</motion.h2>
          <ol className="steps">
            {steps.map((s, i) => (
              <motion.li key={s.title} {...reveal} transition={{ ...reveal.transition, delay: i * 0.1 }}>
                <span className="step-number">{String(i + 1).padStart(2, '0')}</span>
                <h3>{s.href ? <a href={s.href} className="link">{s.title}</a> : s.title}</h3>
                <p>{s.text}</p>
              </motion.li>
            ))}
          </ol>
        </section>

        {!!users && (
          <section className="section container join">
            <motion.h2 {...reveal} onViewportEnter={() => animate(counted, users, { duration: 1.6, ease: [0.22, 1, 0.36, 1] })}>
              Join <motion.span className="join-count">{shownUsers}</motion.span> {users === 1 ? 'user' : 'users'}.
            </motion.h2>
            <motion.p className="lead" {...reveal} transition={{ ...reveal.transition, delay: 0.08 }}>
              Making music in Vybrant on Linux and Windows, each with a free license of their own.
            </motion.p>
            <motion.div className="cta" {...reveal} transition={{ ...reveal.transition, delay: 0.16 }}>
              <a href="/account" className="btn btn-primary">
                Get your free license
              </a>
            </motion.div>
          </section>
        )}

        <section id="pricing" className="section container">
          <motion.div className="price-card" {...reveal}>
            <p className="label">Pricing</p>
            <p className="price">
              Free<span>€0</span>
            </p>
            <ul className="mono">
              <li>Every feature</li>
              <li>One license per account</li>
              <li>Works offline</li>
            </ul>
            <a href={download} className="btn btn-primary">
              Download for {osName[buildOs]}
            </a>
          </motion.div>
        </section>
      </main>
      <Footer />
    </>
  );
}
