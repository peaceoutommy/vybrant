import { StrictMode, type ReactNode } from 'react';
import { ClerkProvider } from '@clerk/react';
import { MotionConfig } from 'motion/react';
import { Landing } from './Landing';
import { Account } from './Account';
import { Download } from './Download';
import { NotFound } from './NotFound';
import { Changelog } from './Changelog';
import { Nav } from './Nav';
import { pageFor } from './pages';

const key = import.meta.env.VITE_CLERK_PUBLISHABLE_KEY;
if (!key) throw new Error('VITE_CLERK_PUBLISHABLE_KEY is not set (see .env.example)');

// Clerk's sign-in and account widgets in the site's colours
const appearance = {
  variables: {
    colorPrimary: '#eef1f5',
    colorPrimaryForeground: '#05070a',
    colorBackground: '#0c0f14',
    colorForeground: '#eef1f5',
    colorMutedForeground: '#8b94a1',
    colorNeutral: '#eef1f5',
    colorInput: '#12161d',
    colorInputForeground: '#eef1f5',
    borderRadius: '10px',
    fontFamily: 'Geist, ui-sans-serif, system-ui, sans-serif',
  },
};

// The page for a path, in the browser (main.tsx) and in the build's prerender (entry-server.tsx)
export function App({ path, children }: { path: string; children?: ReactNode }) {
  const page = { '/': <Landing />, '/download': <Download />, '/changelog': <Changelog />, '/account': <Account />, '/404': <NotFound /> }[pageFor(path)];
  return (
    <StrictMode>
      <ClerkProvider publishableKey={key} afterSignOutUrl="/" signInFallbackRedirectUrl="/account" appearance={appearance}>
        <MotionConfig reducedMotion="never">
          {/* drawn once for the whole site: switching pages keeps it, and the signed-in user, as they are */}
          <Nav />
          {page}
          {children}
        </MotionConfig>
      </ClerkProvider>
    </StrictMode>
  );
}
