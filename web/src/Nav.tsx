import { Show, UserButton } from '@clerk/react';

export function Footer() {
  return (
    <footer className="footer container">
      <span>© {new Date().getFullYear()} Vybrant</span>
    </footer>
  );
}

export function Nav() {
  return (
    <header className="nav">
      <div className="container nav-row">
        <a href="/" className="brand" aria-label="Vybrant home">
          <img src="/logo.png" alt="" width={26} height={26} />
          <span>VYBRANT</span>
        </a>
        <nav className="nav-links" aria-label="Sections">
          <a href="/#features">Features</a>
          <a href="/#pricing">Pricing</a>
          <a href="/download">Download</a>
        </nav>
        <div className="nav-actions">
          <Show when="signed-out">
            <a href="/account" className="btn btn-ghost btn-sm">Sign in</a>
          </Show>
          <Show when="signed-in">
            <a href="/account" className="btn btn-ghost btn-sm">Your license</a>
            <UserButton />
          </Show>
        </div>
      </div>
    </header>
  );
}
