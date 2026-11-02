import { useEffect, useRef, useState } from 'react';
import { AnimatePresence, animate, motion, useMotionValue, useScroll, useTransform } from 'motion/react';
import { Footer } from './Nav';
import { buildOs, osName } from './platform';
import { useLatestRelease } from './release';
import { Intro, reveal } from './Intro';

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

// Feature tiles: the ones with a picture show that part of the app (web/scripts/showcase.py cuts them from its shots)
const tiles = [
  {
    area: 'pattern',
    title: 'Patterns and piano roll',
    text: 'Sketch beats in the step sequencer, then play parts into the piano roll, with the other instruments faintly behind the one you edit.',
    img: '/feature-pattern.webp',
    alt: 'The channel rack over the piano roll: drum steps, bass, chords and a lead melody',
  },
  {
    area: 'mixer',
    title: 'Mixer',
    text: 'Inserts, sends and sidechains, buses, freeze and delay compensation.',
    img: '/feature-mixer.webp',
    alt: 'Mixer strips with their effects, and the send from the chords to the reverb',
  },
  {
    area: 'playlist',
    title: 'Playlist',
    text: 'Arrange patterns, audio and automation on as many tracks as the song needs.',
    img: '/feature-playlist.webp',
    alt: 'A song arranged in the playlist: drums, bass, chords, lead, an audio loop and two automation lanes',
  },
  {
    area: 'sound',
    title: 'Built-in sound',
    text: 'Samplers, drums and three synths, plus EQ, dynamics, delay, reverb and modulation.',
    img: '/feature-eq.webp',
    alt: "The parametric EQ's curve over a live spectrum",
  },
] as const;
const takes = [[0.08, 0.34], [0.3, 0.71], [0.62, 0.95]];   // the drawing on the Recording tile: the part of each take that is used

const steps = [
  { title: 'Download Vybrant', text: 'For 64-bit Linux and Windows, the same file for every update.', href: '/download' },
  { title: 'Get your license', text: 'Sign in with Google or your email; one free license per account.', href: '/account' },
  { title: 'Make your first track', text: 'Paste your license in when Vybrant asks, then start making music. It works offline from then on.' },
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
      <main>
        <section className="hero container">
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

        <motion.div className="container strip" {...reveal} aria-label="Plugin formats and systems">
          <span className="strip-label">Plugins</span>
          {['VST3', 'VST2', 'CLAP', 'LV2'].map((f) => (
            <span key={f}>{f}</span>
          ))}
          <i aria-hidden />
          <span className="strip-label">Runs on</span>
          <span>Linux</span>
          <span>Windows</span>
        </motion.div>

        <section id="features" className="section container">
          <Intro
            label="What's inside"
            title="Everything a track needs."
            text="Write, arrange, record and mix in one app, with instruments and effects built in and your own plugins alongside them."
          />
          <div className="bento">
            {tiles.map((t, i) => (
              <motion.article key={t.area} className={`tile tile-${t.area}`} {...reveal} transition={{ ...reveal.transition, delay: (i % 2) * 0.08 }}>
                <h3>{t.title}</h3>
                <p>{t.text}</p>
                <div className="tile-img">
                  <img src={t.img} alt={t.alt} loading="lazy" />
                </div>
              </motion.article>
            ))}
            <motion.article className="tile tile-plugins" {...reveal}>
              <h3>Your plugins</h3>
              <p>Each one can run in its own process, so a crash stays contained and the song keeps playing.</p>
              <ul className="chips" aria-label="Plugin formats">
                {['VST3', 'VST2', 'CLAP', 'LV2'].map((f) => (
                  <li key={f}>{f}</li>
                ))}
              </ul>
            </motion.article>
            <motion.article className="tile tile-record" {...reveal} transition={{ ...reveal.transition, delay: 0.08 }}>
              <h3>Recording</h3>
              <p>Loop recording with takes and comping, count-in, punch in and out.</p>
              <div className="takes" aria-hidden>
                {takes.map(([from, to], i) => (
                  <div key={i} className="take">
                    <b>Take {i + 1}</b>
                    <div className="take-lane">
                      <span style={{ left: `${from * 100}%`, width: `${(to - from) * 100}%` }} />
                    </div>
                  </div>
                ))}
              </div>
            </motion.article>
            <motion.article className="tile tile-export" {...reveal} transition={{ ...reveal.transition, delay: 0.16 }}>
              <h3>Export</h3>
              <p>Mixdowns, stems and MIDI files, at any sample rate.</p>
              <ul className="files" aria-hidden>
                <li>Song.wav</li>
                <li>Song - Drums.wav</li>
                <li>Song - Bass.wav</li>
                <li>Song.mid</li>
              </ul>
            </motion.article>
          </div>
        </section>

        <section id="license" className="section container">
          <Intro label="Get started" title="Three steps to your first track." text="No trial, no card, no catch: one free license per account." />
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
          <motion.div className="pricing" {...reveal}>
            <div>
              <p className="label">Pricing</p>
              <p className="price">€0</p>
              <p className="pricing-sub">Free, with everything in it.</p>
            </div>
            <ul className="pricing-list">
              <li>Every instrument, effect and feature</li>
              <li>One license per account, yours to keep</li>
              <li>Checked on your machine: works offline</li>
              <li>Updates offered inside the app</li>
            </ul>
            <div className="pricing-cta">
              <a href={download} className="btn btn-primary">
                Download for {osName[buildOs]}
              </a>
              <a href="/account" className="btn btn-ghost">
                Get your free license
              </a>
            </div>
          </motion.div>
        </section>

        <section className="section container closing">
          <motion.h2 {...reveal} onViewportEnter={() => users && animate(counted, users, { duration: 1.6, ease: [0.22, 1, 0.36, 1] })}>
            {users ? (
              <>
                Join <motion.span className="join-count">{shownUsers}</motion.span> {users === 1 ? 'user' : 'users'}.
              </>
            ) : (
              'Make your next track here.'
            )}
          </motion.h2>
          <motion.p className="lead" {...reveal} transition={{ ...reveal.transition, delay: 0.08 }}>
            Free on Linux and Windows: every feature, one license per account.
          </motion.p>
          <motion.div className="cta" {...reveal} transition={{ ...reveal.transition, delay: 0.16 }}>
            <a href={download} className="btn btn-primary">
              Download for {osName[buildOs]}
            </a>
            <a href="/account" className="btn btn-ghost">
              Get your free license
            </a>
          </motion.div>
        </section>
      </main>
      <Footer />
    </>
  );
}
