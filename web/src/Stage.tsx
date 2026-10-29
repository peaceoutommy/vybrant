import { useRef, useState, type CSSProperties, type PointerEvent } from 'react';
import { motion, useInView, useMotionValue, useReducedMotion, useScroll, useSpring, useTransform } from 'motion/react';

// Crops of the app itself (`vybrant --demo --screenshot`), each view in another of its theme presets
const cards = [
  { src: '/cards/mixer.webp', name: 'Mixer', theme: 'Obsidian', alt: 'Mixer channel strips with their faders', w: 480, h: 640 },
  { src: '/cards/pianoroll.webp', name: 'Piano roll', theme: 'Vybrant', alt: 'Piano roll notes beside the keyboard, velocity lane below', w: 420, h: 560 },
  { src: '/cards/rack.webp', name: 'Channel rack', theme: 'Ocean', alt: 'Channel rack steps for kick, clap and hat', w: 378, h: 504 },
  { src: '/cards/synth.webp', name: 'Synth', theme: 'Ember', alt: "The built-in synth's oscillator knobs", w: 399, h: 532 },
  { src: '/cards/playlist.webp', name: 'Playlist', theme: 'Vybrant', alt: 'Pattern clips arranged in the playlist', w: 382, h: 510 },
];
const ring = [...cards, ...cards];   // twice round, so the ring has no gap
const faces = ['front', 'back', 'right', 'left', 'top', 'bottom'];
const ease = [0.76, 0, 0.24, 1] as const;
// Cuts take their cue from the window: one on the window's right edge starts zero-wide there and never counts as in view itself
const cut = (n: number) => ({ hidden: { scaleX: 0 }, show: { scaleX: 1, transition: { duration: 0.7, delay: 0.5 + n * 0.06, ease } } });

// The stepped frame: a pastel window with blocks cut into its edges, a ring of app views and a cube with the mark
export function Stage() {
  const ref = useRef<HTMLElement>(null);
  const reduce = useReducedMotion();
  const inView = useInView(ref);
  const [playing, setPlaying] = useState(true);

  const { scrollYProgress } = useScroll({ target: ref, offset: ['start end', 'end start'] });
  const px = useMotionValue(0), py = useMotionValue(0);   // pointer, -0.5..0.5 over the stage
  const sx = useSpring(px, { stiffness: 60, damping: 18 }), sy = useSpring(py, { stiffness: 60, damping: 18 });
  const tilt = useTransform(() => 16 - scrollYProgress.get() * 22 - sy.get() * 10);   // the ring leans back, then levels as it passes
  const turn = useTransform(sx, [-0.5, 0.5], [-12, 12]);
  const cubeX = useTransform(sy, [-0.5, 0.5], [28, -28]);
  const cubeY = useTransform(sx, [-0.5, 0.5], [-34, 34]);
  const lift = useTransform(scrollYProgress, [0, 1], [60, -60]);

  const point = (e: PointerEvent) => {
    const r = e.currentTarget.getBoundingClientRect();
    px.set((e.clientX - r.left) / r.width - 0.5);
    py.set((e.clientY - r.top) / r.height - 0.5);
  };
  const rest = () => (px.set(0), py.set(0));

  return (
    <section ref={ref} className="stage container" aria-label="Views of Vybrant" onPointerMove={reduce ? undefined : point} onPointerLeave={rest}>
      <p className="frame-labels" aria-hidden>
        <span>Fig.01 — Inside the studio</span>
        <span>VYB.DAW</span>
        <span>©{new Date().getFullYear()}</span>
      </p>
      <div className="frame">
        <motion.div
          className="window"
          initial="hidden"
          whileInView="show"
          viewport={{ once: true, margin: '-60px' }}
          variants={{ hidden: { clipPath: `inset(0 0 ${reduce ? 0 : 100}% 0)` }, show: { clipPath: 'inset(0 0 0% 0)', transition: { duration: 1.1, ease } } }}
        >
          <div className="ring-view">
            <motion.div className="ring-tilt" style={reduce ? undefined : { rotateX: tilt, rotateY: turn }}>
              <ul className={`ring${playing && inView ? '' : ' paused'}`}>
                {ring.map((c, i) => (
                  <li key={i} style={{ '--i': i } as CSSProperties} aria-hidden={i >= cards.length || undefined}>
                    <p className="card-label">
                      <span>{String((i % cards.length) + 1).padStart(2, '0')} {c.name}</span>
                      <span>{c.theme}</span>
                    </p>
                    <img src={c.src} alt={i < cards.length ? c.alt : ''} width={c.w} height={c.h} loading="lazy" decoding="async" />
                  </li>
                ))}
              </ul>
            </motion.div>
          </div>

          {[1, 2, 3, 4, 5, 6, 7].map((n) => (
            <motion.span key={n} className={`cut cut-${n}`} aria-hidden variants={cut(n)}>
              {n === 1 && 'Pattern 1 · 4/4'}
              {n === 4 && '001:1:000 · 140.0 BPM'}
            </motion.span>
          ))}

          {!reduce && (
            <button className="transport" onClick={() => setPlaying(!playing)} aria-label={playing ? 'Stop the carousel' : 'Play the carousel'}>
              <span aria-hidden>{playing ? '■' : '▶'}</span> {playing ? 'Stop' : 'Play'}
            </button>
          )}
        </motion.div>

        <div className="cube-wrap" aria-hidden>
          <motion.div className="cube-tilt" style={reduce ? undefined : { rotateX: cubeX, rotateY: cubeY, y: lift }}>
            <div className={`cube${playing && inView ? '' : ' paused'}`}>
              {faces.map((f) => (
                <span key={f} className={`face ${f}`} />
              ))}
            </div>
          </motion.div>
        </div>

        {[1, 2, 3, 4].map((n) => (
          <span key={n} className={`bar bar-${n}`} aria-hidden />
        ))}
      </div>
      <p className="frame-labels" aria-hidden>
        <span>ヴァイブラント</span>
        <span>Mixer / Piano roll / Channel rack / Synth / Playlist</span>
      </p>
    </section>
  );
}
