import type { ReactNode } from 'react';
import { motion } from 'motion/react';

// Reveal as it scrolls into view, once
export const reveal = {
  initial: { opacity: 0, y: 24 },
  whileInView: { opacity: 1, y: 0 },
  viewport: { once: true, margin: '-80px' },
  transition: { duration: 0.7, ease: [0.22, 1, 0.36, 1] },
} as const;

// A section's heading: label and title on the left; on the right a line of copy, or a control (children)
export function Intro({ label, title, text, children }: { label: string; title: string; text?: string; children?: ReactNode }) {
  return (
    <div className="intro">
      <div>
        <motion.p className="label" {...reveal}>
          {label}
        </motion.p>
        <motion.h2 {...reveal}>{title}</motion.h2>
      </div>
      <motion.div className="intro-text" {...reveal} transition={{ ...reveal.transition, delay: 0.08 }}>
        {text ?? children}
      </motion.div>
    </div>
  );
}
