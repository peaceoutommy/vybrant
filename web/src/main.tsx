import { useEffect, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { App } from './App';
import { CookieNotice } from './CookieNotice';
import { pageFor, pages } from './pages';
import './styles.css';

// Links between the site's pages switch the page in place instead of loading a new one, so the bar and the signed-in
// user stay where they are. Each page also exists as its own HTML file (scripts/prerender.mjs) for a fresh visit.
function Site() {
  const [path, setPath] = useState(location.pathname);
  const pushed = useRef(false);   // reached by a link: start at the top (or its #section); back / forward leave scrolling to the browser
  useEffect(() => {
    const onClick = (e: MouseEvent) => {
      const a = (e.target as Element).closest('a');
      if (!a || e.defaultPrevented || e.button !== 0 || e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
      if (a.target || a.hasAttribute('download') || a.origin !== location.origin || a.pathname === location.pathname) return;
      e.preventDefault();
      history.pushState(null, '', a.href);
      pushed.current = true;
      setPath(location.pathname);
    };
    const onPop = () => setPath(location.pathname);
    document.addEventListener('click', onClick);
    addEventListener('popstate', onPop);
    return () => {
      document.removeEventListener('click', onClick);
      removeEventListener('popstate', onPop);
    };
  }, []);
  useEffect(() => {
    document.title = pages[pageFor(path)].title;
    if (!pushed.current) return;
    pushed.current = false;
    const section = location.hash && document.getElementById(location.hash.slice(1));
    if (section) section.scrollIntoView();
    else scrollTo(0, 0);
  }, [path]);
  return (
    <App path={path}>
      <CookieNotice />
    </App>
  );
}

// The build leaves each page's HTML in #root for crawlers; the live page replaces it
createRoot(document.getElementById('root')!).render(<Site />);
