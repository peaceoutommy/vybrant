import { useState } from 'react';
import { AnimatePresence, motion, useMotionValueEvent, useScroll } from 'motion/react';
import { Show, UserButton } from '@clerk/react';
import { buildOs, osName } from './platform';
import { useLatestRelease } from './release';

export function Footer() {
  return (
    <footer className="footer container">
      <span>© {new Date().getFullYear()} Vybrant</span>
    </footer>
  );
}

const links = [
  { label: 'Features', href: '/#features', section: 'features' },
  { label: 'Pricing', href: '/#pricing', section: 'pricing' },
  { label: 'Download', href: '/download', section: '' },
];
const spring = { type: 'spring', stiffness: 420, damping: 38 } as const;

// Open across the top of the page; once it scrolls, it gathers into a floating pill that marks the section in view,
// fills a hairline with the scroll, and offers the download the hero no longer shows
export function Nav() {
  const { scrollY, scrollYProgress } = useScroll();
  const [compact, setCompact] = useState(false);
  const [active, setActive] = useState(location.pathname === '/download' ? 'download' : '');
  const [hover, setHover] = useState<string | null>(null);
  const [open, setOpen] = useState(false);   // the menu, on narrow screens
  const download = useLatestRelease().release?.[buildOs]?.url ?? '/download';

  useMotionValueEvent(scrollY, 'change', (y) => {
    setCompact(y > 48);
    if (location.pathname !== '/') return;
    let current = '';   // the last section whose top has passed the upper third of the window
    for (const l of links)
      if (l.section && (document.getElementById(l.section)?.getBoundingClientRect().top ?? Infinity) < innerHeight / 3) current = l.section;
    setActive(current);
  });

  const isActive = (l: (typeof links)[number]) => (l.section ? active === l.section : active === 'download');
  const showCta = compact && location.pathname !== '/download';

  return (
    <header className="nav">
      <motion.div layout transition={spring} className={`nav-bar${compact ? ' compact' : ''}${open ? ' open' : ''}`}
        style={{ borderRadius: compact || open ? 26 : 0 }}>
        <div className="nav-row">
          <motion.a layout="position" href="/" className="brand" aria-label="Vybrant home">
            <img src="/logo.png" alt="" width={26} height={26} />
            <AnimatePresence initial={false}>
              {!compact && (
                <motion.span key="name" initial={{ opacity: 0, width: 0 }} animate={{ opacity: 1, width: 'auto' }} exit={{ opacity: 0, width: 0 }}>
                  VYBRANT
                </motion.span>
              )}
            </AnimatePresence>
          </motion.a>

          <motion.nav layout="position" className="nav-links" aria-label="Sections" onMouseLeave={() => setHover(null)}>
            {links.map((l) => (
              <a key={l.label} href={l.href} aria-current={isActive(l) ? 'page' : undefined} className={isActive(l) ? 'on' : undefined}
                onMouseEnter={() => setHover(l.label)}>
                {hover === l.label && <motion.span layoutId="nav-hover" className="nav-hover" transition={spring} />}
                <span>{l.label}</span>
                {isActive(l) && <motion.span layoutId="nav-dot" className="nav-dot" transition={spring} />}
              </a>
            ))}
          </motion.nav>

          <motion.div layout="position" className="nav-actions">
            <AnimatePresence initial={false}>
              {showCta && (
                <motion.a key="cta" href={download} className="btn btn-primary btn-sm nav-cta" initial={{ opacity: 0, x: 12, scale: 0.9 }}
                  animate={{ opacity: 1, x: 0, scale: 1 }} exit={{ opacity: 0, x: 12, scale: 0.9 }} transition={spring}
                  title={`Download for ${osName[buildOs]}`}>
                  Download
                </motion.a>
              )}
            </AnimatePresence>
            <Show when="signed-out">
              <a href="/account" className="btn btn-ghost btn-sm nav-account">Sign in</a>
            </Show>
            <Show when="signed-in">
              <a href="/account" className="btn btn-ghost btn-sm nav-account">Your license</a>
              <UserButton />
            </Show>
            <button className="nav-menu" aria-label="Menu" aria-expanded={open} onClick={() => setOpen((o) => !o)}>
              <span /><span />
            </button>
          </motion.div>
        </div>

        <AnimatePresence initial={false}>
          {open && (
            <motion.nav key="menu" className="nav-sheet" aria-label="Menu" initial={{ opacity: 0, height: 0 }} animate={{ opacity: 1, height: 'auto' }}
              exit={{ opacity: 0, height: 0 }} transition={spring}>
              {links.map((l) => (
                <a key={l.label} href={l.href} onClick={() => setOpen(false)} aria-current={isActive(l) ? 'page' : undefined}>
                  {l.label}
                </a>
              ))}
              <a href="/account" onClick={() => setOpen(false)}>Account</a>
            </motion.nav>
          )}
        </AnimatePresence>

        {compact && <motion.span className="nav-progress" style={{ scaleX: scrollYProgress }} aria-hidden />}
      </motion.div>
    </header>
  );
}
