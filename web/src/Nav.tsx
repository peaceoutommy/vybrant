import { useEffect, useState } from 'react';
import { AnimatePresence, motion } from 'motion/react';
import { Show, UserButton } from '@clerk/react';

export function Footer() {
  return (
    <footer className="footer container">
      <div className="footer-brand">
        <a href="/" className="brand" aria-label="Vybrant home">
          <img src="/logo.png" alt="" width={22} height={22} />
          <span>VYBRANT</span>
        </a>
        <p>A complete music studio for Linux and Windows.</p>
      </div>
      <nav className="footer-links" aria-label="Footer">
        <div>
          <p className="label">Product</p>
          <a href="/#features">Features</a>
          <a href="/#pricing">Pricing</a>
          <a href="/download">Download</a>
        </div>
        <div>
          <p className="label">Account</p>
          <a href="/account">Your license</a>
        </div>
      </nav>
      <p className="footer-legal">© {new Date().getFullYear()} Vybrant</p>
    </footer>
  );
}

const links = [
  { label: 'Features', href: '/#features' },
  { label: 'Pricing', href: '/#pricing' },
  { label: 'Download', href: '/download' },
];
const here = (href: string) => (typeof location !== 'undefined' && location.pathname === href ? 'page' : undefined);   // none in the prerender

// A see-through bar across the top: the name on the left, the links on the right
export function Nav() {
  const [open, setOpen] = useState(false);   // the menu, on narrow screens: it covers the screen under the bar
  useEffect(() => {
    document.body.style.overflow = open ? 'hidden' : '';   // the page stays put under it
  }, [open]);

  return (
    <header className={open ? 'nav open' : 'nav'}>
      <div className="nav-row">
        <a href="/" className="brand" aria-label="Vybrant home">
          <img src="/logo.png" alt="" width={26} height={26} />
          <span>VYBRANT</span>
        </a>
        <nav className="nav-links" aria-label="Sections">
          {links.map((l) => (
            <a key={l.label} href={l.href} aria-current={here(l.href)}>
              {l.label}
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
          <motion.nav key="menu" className="nav-sheet" aria-label="Menu" initial={{ opacity: 0, y: -8 }} animate={{ opacity: 1, y: 0 }}
            exit={{ opacity: 0, y: -8 }} transition={{ duration: 0.3, ease: [0.22, 1, 0.36, 1] }}>
            {links.map((l) => (
              <a key={l.label} href={l.href} onClick={() => setOpen(false)} aria-current={here(l.href)}>
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
