import { useEffect, useState } from 'react';
import { Show, SignIn, useAuth, useUser } from '@clerk/react';
import { AnimatePresence, motion } from 'motion/react';

const API = import.meta.env.VITE_API_URL ?? '';

// GET: the account's license (null before one was made); POST: make it (the server returns the existing one if there is one)
async function licenseCall(method: 'GET' | 'POST', token: string | null): Promise<string | null> {
  const res = await fetch(`${API}/api/license`, { method, headers: { Authorization: `Bearer ${token}` } });
  if (!res.ok) throw new Error(`The license server answered ${res.status}. Try again in a moment.`);
  return (await res.json()).license;
}

export function Account() {
  return (
    <>
      <div className="glow" aria-hidden />
      <main className="account container">
        <Show when="signed-out">
          <div className="signin">
            <SignIn routing="hash" />
          </div>
        </Show>
        <Show when="signed-in">
          <LicensePanel />
        </Show>
      </main>
    </>
  );
}

function LicensePanel() {
  const { getToken } = useAuth();
  const { user } = useUser();
  const [license, setLicense] = useState<string | null | undefined>(undefined);   // undefined: still asking
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [copied, setCopied] = useState(false);

  const run = async (method: 'GET' | 'POST') => {
    setBusy(true);
    setError('');
    try {
      setLicense(await licenseCall(method, await getToken()));
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  useEffect(() => {
    run('GET');
  }, []);

  const copy = async () => {
    await navigator.clipboard.writeText(license!);
    setCopied(true);
    setTimeout(() => setCopied(false), 1800);
  };

  return (
    <motion.section className="panel" initial={{ opacity: 0, y: 16 }} animate={{ opacity: 1, y: 0 }} transition={{ duration: 0.6 }}>
      <p className="label">{user?.primaryEmailAddress?.emailAddress}</p>
      <h1 className="panel-title">Your license</h1>
      {error && (
        <p className="error" role="alert">
          {error}
        </p>
      )}
      <AnimatePresence mode="wait">
        {license === undefined && !error && (
          <motion.p key="loading" className="muted" exit={{ opacity: 0 }}>
            Looking for your license…
          </motion.p>
        )}
        {license === null && (
          <motion.div key="none" initial={{ opacity: 0 }} animate={{ opacity: 1 }} exit={{ opacity: 0 }}>
            <p className="muted">Your account comes with one free license. Generate it once and use it on your computers.</p>
            <button className="btn btn-primary" onClick={() => run('POST')} disabled={busy}>
              {busy ? 'Generating…' : 'Generate license'}
            </button>
          </motion.div>
        )}
        {license && (
          <motion.div
            key="license"
            initial={{ opacity: 0, filter: 'blur(8px)' }}
            animate={{ opacity: 1, filter: 'blur(0px)' }}
            transition={{ duration: 0.6 }}
          >
            <pre className="key" aria-label="License key">
              {license}
            </pre>
            <div className="cta" aria-live="polite">
              <button className="btn btn-primary" onClick={copy}>
                {copied ? 'Copied' : 'Copy license'}
              </button>
              <a href="/download" className="btn btn-ghost">
                Download Vybrant
              </a>
            </div>
            <p className="muted small">
              Open Vybrant and paste it when asked, or later under Help → License…
            </p>
          </motion.div>
        )}
      </AnimatePresence>
    </motion.section>
  );
}
