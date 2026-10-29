import { useState } from 'react';

const KEY = 'cookie-notice-seen';
const seen = () => {
  try {
    return localStorage.getItem(KEY) === '1';
  } catch {
    return false;   // storage blocked: show it, it is only a notice
  }
};

// The site only sets the cookies sign-in needs (Clerk's session), which need no consent; this says so, once
export function CookieNotice() {
  const [open, setOpen] = useState(() => !seen());
  if (!open) return null;
  const close = () => {
    try {
      localStorage.setItem(KEY, '1');
    } catch {
      /* the notice just comes back next visit */
    }
    setOpen(false);
  };
  return (
    <div className="cookie-notice" role="region" aria-label="Cookies">
      <p>
        We only use the cookies that keep you signed in. No tracking, no ads.
      </p>
      <button className="btn btn-primary btn-sm" onClick={close}>
        Got it
      </button>
    </div>
  );
}
