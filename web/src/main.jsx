import { createRoot } from 'react-dom/client'
import { ClerkProvider } from '@clerk/react'
import './index.css'
import App from './App'

// Clerk's sign-in widgets in the site's colours
const appearance = {
  variables: {
    colorPrimary: '#eef1f5',
    colorPrimaryForeground: '#060509',
    colorBackground: '#0c0a12',
    colorForeground: '#eef1f5',
    colorMutedForeground: '#8b8799',
    colorNeutral: '#eef1f5',
    colorInput: '#14111c',
    colorInputForeground: '#eef1f5',
    borderRadius: '12px',
    fontFamily: 'Geist, ui-sans-serif, system-ui, sans-serif',
  },
}

createRoot(document.getElementById('root')).render(
  <ClerkProvider publishableKey={import.meta.env.VITE_CLERK_PUBLISHABLE_KEY} afterSignOutUrl="/#account" appearance={appearance}>
    <App />
  </ClerkProvider>,
)
