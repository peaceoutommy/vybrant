import { Show, UserButton } from '@clerk/react';

export function Nav() {
  return (
    <header className="nav">
      <div className="container nav-row">
        <a href="/" className="brand" aria-label="Tactum home">
          <img src="/logo.svg" alt="" width={26} height={26} />
          <span>TACTUM</span>
        </a>
        <nav className="nav-links" aria-label="Sections">
          <a href="/#features">Features</a>
          <a href="/#license">License</a>
          <a href="/#pricing">Pricing</a>
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
