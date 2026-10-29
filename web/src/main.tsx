import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { ClerkProvider } from '@clerk/react';
import { MotionConfig } from 'motion/react';
import { Landing } from './Landing';
import { Account } from './Account';
import { Download } from './Download';
import './styles.css';

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

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <ClerkProvider publishableKey={key} afterSignOutUrl="/" signInFallbackRedirectUrl="/account" appearance={appearance}>
      <MotionConfig reducedMotion="never">
        {location.pathname.startsWith('/account') ? <Account /> : location.pathname.startsWith('/download') ? <Download /> : <Landing />}
      </MotionConfig>
    </ClerkProvider>
  </StrictMode>,
);
