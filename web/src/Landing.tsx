import { useRef } from 'react';
import { motion, useScroll, useTransform } from 'motion/react';
import { Footer, Nav } from './Nav';

const features = [
  { title: 'Patterns and playlist', text: 'Step sequencer, piano roll and a playlist that arranges patterns, audio and automation.' },
  { title: 'Recording', text: 'Loop recording with takes and comping, count-in and punch in/out.' },
  { title: 'Mixer', text: 'Inserts, sends and sidechains, track freeze and latency-compensated renders.' },
  { title: 'Your plugins', text: 'VST2, VST3, CLAP and LV2. Each can run in its own process, so a crash stays contained.' },
  { title: 'Built-in sound', text: 'Samplers, synths and drums, plus EQ, dynamics, delay, reverb and modulation effects.' },
  { title: 'Export', text: 'Mixdowns, stems and MIDI files, at any sample rate.' },
];

const steps = [
  { title: 'Download Vybrant', text: 'One file for 64-bit Linux, the same one for every update.', href: '/download' },
  { title: 'Get your license', text: 'Sign in with Google or your email; one free license per account.', href: '/account' },
  { title: 'Paste it into Vybrant', text: 'Vybrant checks it on your machine. No connection needed after that.' },
];

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

  return (
    <>
      <div className="glow" aria-hidden />
      <Nav />
      <main>
        <section className="hero container">
          <motion.p className="label" {...reveal}>
            Digital audio workstation · Linux
          </motion.p>
          <motion.h1 {...reveal} transition={{ ...reveal.transition, delay: 0.08 }}>
            A studio you
            <br />
            can <em>feel.</em>
          </motion.h1>
          <motion.p className="lead" {...reveal} transition={{ ...reveal.transition, delay: 0.16 }}>
            Vybrant is a complete music studio for Linux. Sequence patterns, play parts into the piano roll, mix with sends and
            sidechains, and run your own plugins.
          </motion.p>
          <motion.div className="cta" {...reveal} transition={{ ...reveal.transition, delay: 0.24 }}>
            <a href="/download" className="btn btn-primary">
              Download for Linux
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
          <motion.div
            className="shot"
            style={{ rotateX: tilt }}
            initial={{ opacity: 0, y: 40 }}
            animate={{ opacity: 1, y: 0 }}
            transition={{ duration: 1, delay: 0.35, ease: [0.22, 1, 0.36, 1] }}
          >
            <img src="/vybrant.webp" alt="Vybrant's playlist with patterns arranged over eight bars" width={1600} height={640} />
          </motion.div>
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
            <a href="/download" className="btn btn-primary">
              Download for Linux
            </a>
          </motion.div>
        </section>
      </main>
      <Footer />
    </>
  );
}
