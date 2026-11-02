import { createRoot } from 'react-dom/client';
import { App } from './App';
import { CookieNotice } from './CookieNotice';
import './styles.css';

// The build leaves each page's HTML in #root for crawlers (scripts/prerender.mjs); the live page replaces it
createRoot(document.getElementById('root')!).render(
  <App path={location.pathname}>
    <CookieNotice />
  </App>,
);
