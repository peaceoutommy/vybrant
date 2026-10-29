import { useRef, type CSSProperties } from 'react';
import { motion, useReducedMotion, useScroll, useTransform } from 'motion/react';
import { Footer, Nav } from './Nav';
import { Stage } from './Stage';

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

// The demo song's drums, as the channel rack shows them
const pattern = [
  { name: 'Kick', color: '#ec7262', steps: 'x...x...x...x...' },
  { name: 'Clap', color: '#e3a33c', steps: '....x.......x...' },
  { name: 'Hat', color: '#7fd37f', steps: 'x.x.x.x.x.x.x.x.' },
  { name: 'Snare', color: '#6aa8ec', steps: '.......x......x.' },
];

// A waveform for the price card: a few sines under a swell
const wave = Array.from({ length: 151 }, (_, i) => {
  const y = 30 + Math.sin(i * 0.83) * Math.sin(i * 0.19) * 26 * Math.sin((i / 150) * Math.PI);
  return `${i ? 'L' : 'M'}${i * 4} ${y.toFixed(1)}`;
}).join('');

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
  const reduce = useReducedMotion();

  return (
    <>
      <div className="glow" aria-hidden />
      <Nav />
      <Playhead />
      <main>
        <section className="hero container">
          <motion.p className="label" {...reveal}>
            Digital audio workstation · Linux · Windows
          </motion.p>
          <motion.h1 {...reveal} transition={{ ...reveal.transition, delay: 0.08 }}>
            A studio you
            <br />
            can <em>feel.</em>
          </motion.h1>
          <motion.p className="lead" {...reveal} transition={{ ...reveal.transition, delay: 0.16 }}>
            Vybrant is a complete music studio for Linux and Windows. Sequence patterns, play parts into the piano roll, mix with sends and
            sidechains, and run your own plugins.
          </motion.p>
          <motion.div className="cta" {...reveal} transition={{ ...reveal.transition, delay: 0.24 }}>
            <a href="/download" className="btn btn-primary">
              Download
            </a>
            <a href="/account" className="btn btn-ghost">
              Get your free license
            </a>
          </motion.div>
          <motion.p className="caption" {...reveal} transition={{ ...reveal.transition, delay: 0.3 }}>
            Free · One license per account · Works offline
          </motion.p>
        </section>

        <Stage />

        <section id="features" className="section container">
          <div className="section-head">
            <div>
              <motion.p className="label" {...reveal}>
                What's inside
              </motion.p>
              <motion.h2 {...reveal}>Everything a track needs.</motion.h2>
            </div>
            <motion.div {...reveal} transition={{ ...reveal.transition, delay: 0.12 }}>
              <Sequencer />
            </motion.div>
          </div>
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

        <div className="container shot-wrap" ref={shot}>
          <p className="frame-labels" aria-hidden>
            <span>Fig.02 — Playlist</span>
            <span>001:1:000 → 009:1:000</span>
          </p>
          <motion.div className="shot" style={{ rotateX: reduce ? 0 : tilt }} {...reveal}>
            <img loading="lazy" src="/vybrant.webp" alt="Vybrant's playlist with patterns arranged over eight bars" width={1600} height={640} />
          </motion.div>
        </div>

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

        <section id="pricing" className="section container">
          <motion.div className="price-card" {...reveal}>
            <p className="label">Pricing</p>
            <svg className="wave" viewBox="0 0 600 60" preserveAspectRatio="none" aria-hidden>
              <defs>
                <linearGradient id="wave-ink" x1="0" x2="1">
                  <stop offset="0" stopColor="#bfeedd" />
                  <stop offset="0.5" stopColor="#d6c4f6" />
                  <stop offset="1" stopColor="#a7b5f6" />
                </linearGradient>
              </defs>
              <motion.path
                d={wave}
                initial={{ pathLength: 0 }}
                whileInView={{ pathLength: 1 }}
                viewport={{ once: true }}
                transition={{ duration: 1.8, ease: [0.22, 1, 0.36, 1] }}
              />
            </svg>
            <p className="price">
              Free<span>€0</span>
            </p>
            <ul className="mono">
              <li>Every feature</li>
              <li>One license per account</li>
              <li>Works offline</li>
            </ul>
            <a href="/download" className="btn btn-primary">
              Download
            </a>
          </motion.div>
        </section>
      </main>
      <Footer />
    </>
  );
}

// Step lights run along at 140 BPM, pure CSS (styles.css: .seq)
function Sequencer() {
  return (
    <div className="seq" aria-hidden>
      <p className="seq-head">
        <span>Pattern 1</span>
        <span>140.0 BPM · 4/4</span>
      </p>
      {pattern.map((t) => (
        <div key={t.name} className="seq-row" style={{ '--c': t.color } as CSSProperties}>
          <span>{t.name}</span>
          {[...t.steps].map((s, i) => (
            <i key={i} className={s === 'x' ? 'on' : undefined} style={{ '--i': i } as CSSProperties} />
          ))}
        </div>
      ))}
    </div>
  );
}

// A song position counter in the corner: the page is 32 bars long and scrolling moves the playhead
function Playhead() {
  const { scrollYProgress } = useScroll();
  const position = useTransform(scrollYProgress, (p) => {
    const t = Math.round(p * 32 * 4 * 960);   // ticks, 960 per beat
    return `${String(Math.floor(t / 3840) + 1).padStart(3, '0')}:${(Math.floor(t / 960) % 4) + 1}:${String(t % 960).padStart(3, '0')}`;
  });
  return (
    <div className="playhead" aria-hidden>
      <span className="rec" />
      <motion.span>{position}</motion.span>
      <span>140.0 BPM</span>
      <motion.i style={{ scaleX: scrollYProgress }} />
    </div>
  );
}
