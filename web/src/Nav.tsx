import { useState } from 'react';
import { AnimatePresence, motion, useMotionValueEvent, useScroll } from 'motion/react';
import { Show, UserButton } from '@clerk/react';

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

// A see-through bar across the top: the name on the left, the links on the right, a dot under the section in view
export function Nav() {
  const { scrollY } = useScroll();
  const [active, setActive] = useState(location.pathname === '/download' ? 'download' : '');
  const [open, setOpen] = useState(false);   // the menu, on narrow screens

  useMotionValueEvent(scrollY, 'change', () => {
    if (location.pathname !== '/') return;
    let current = '';   // the last section whose top has passed the upper third of the window
    for (const l of links)
      if (l.section && (document.getElementById(l.section)?.getBoundingClientRect().top ?? Infinity) < innerHeight / 3) current = l.section;
    setActive(current);
  });
  const isActive = (l: (typeof links)[number]) => (l.section ? active === l.section : active === 'download');

  return (
    <header className="nav">
      <div className="nav-row">
        <a href="/" className="brand" aria-label="Vybrant home">
          <img src="/logo.png" alt="" width={26} height={26} />
          <span>VYBRANT</span>
        </a>
        <nav className="nav-links" aria-label="Sections">
          {links.map((l) => (
            <a key={l.label} href={l.href} aria-current={isActive(l) ? 'page' : undefined} className={isActive(l) ? 'on' : undefined}>
              {l.label}
              {isActive(l) && <motion.span layoutId="nav-dot" className="nav-dot" transition={{ type: 'spring', stiffness: 420, damping: 38 }} />}
            </a>
          ))}
        </nav>
        <div className="nav-actions">
          <Show when="signed-out">
            <a href="/account" className="btn btn-ghost btn-sm nav-account">Sign in</a>
          </Show>
          <Show when="signed-in">
            <a href="/account" className="btn btn-ghost btn-sm nav-account">Your license</a>
            <UserButton />
          </Show>
          <button className="nav-menu" aria-label="Menu" aria-expanded={open} onClick={() => setOpen((o) => !o)}>
            <span />
            <span />
          </button>
        </div>
      </div>
      <AnimatePresence initial={false}>
        {open && (
          <motion.nav key="menu" className="nav-sheet" aria-label="Menu" initial={{ opacity: 0, height: 0 }} animate={{ opacity: 1, height: 'auto' }}
            exit={{ opacity: 0, height: 0 }} transition={{ duration: 0.3, ease: [0.22, 1, 0.36, 1] }}>
            {links.map((l) => (
              <a key={l.label} href={l.href} onClick={() => setOpen(false)} aria-current={isActive(l) ? 'page' : undefined}>
                {l.label}
              </a>
            ))}
            <a href="/account" onClick={() => setOpen(false)}>Account</a>
          </motion.nav>
        )}
      </AnimatePresence>
    </header>
  );
}
