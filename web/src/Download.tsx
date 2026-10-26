import { useEffect, useState } from 'react';
import { motion } from 'motion/react';
import { Footer, Nav } from './Nav';

// The release workflow (.github/workflows/release.yml) publishes the AppImage here on every v* tag
const REPO = 'peaceoutommy/Auralis';
const RELEASES = `https://github.com/${REPO}/releases`;

interface Release {
  version: string;
  date: string;
  size: number;
  url: string;   // the AppImage itself
  notes: string;   // the release page
}

// The latest release, from GitHub's API: null while loading or when there is none (the buttons then open the releases page)
function useLatestRelease() {
  const [release, setRelease] = useState<Release | null>(null);
  const [state, setState] = useState<'loading' | 'ready' | 'none'>('loading');
  useEffect(() => {
    fetch(`https://api.github.com/repos/${REPO}/releases/latest`)
      .then((r) => (r.ok ? r.json() : Promise.reject()))
      .then((r) => {
        const asset = r.assets.find((a: { name: string }) => /x86_64\.AppImage$/.test(a.name));   // the build we make; never another arch
        if (!asset) return setState('none');
        setRelease({ version: r.tag_name.replace(/^v/, ''), date: r.published_at, size: asset.size, url: asset.browser_download_url, notes: r.html_url });
        setState('ready');
      })
      .catch(() => setState('none'));
  }, []);
  return { release, state };
}

const onLinux = /linux/i.test(navigator.userAgent) && !/android/i.test(navigator.userAgent);

const reveal = {
  initial: { opacity: 0, y: 20 },
  animate: { opacity: 1, y: 0 },
  transition: { duration: 0.7, ease: [0.22, 1, 0.36, 1] },
} as const;

export function Download() {
  const { release, state } = useLatestRelease();
  const [copied, setCopied] = useState(false);
  const command = 'chmod +x Tactum-*.AppImage && ./Tactum-*.AppImage';
  const copy = async () => {
    await navigator.clipboard.writeText(command);
    setCopied(true);
    setTimeout(() => setCopied(false), 1800);
  };

  return (
    <>
      <div className="glow" aria-hidden />
      <Nav />
      <main>
        <section className="hero container">
          <motion.p className="label" {...reveal}>
            Download
          </motion.p>
          <motion.h1 {...reveal} transition={{ ...reveal.transition, delay: 0.08 }}>
            Get <em>Tactum.</em>
          </motion.h1>
          <motion.p className="lead" {...reveal} transition={{ ...reveal.transition, delay: 0.16 }}>
            One download for every update. Tactum asks for your license the first time it starts, and it's free with an
            account.
          </motion.p>
          {!onLinux && (
            <motion.p className="notice" {...reveal} transition={{ ...reveal.transition, delay: 0.2 }}>
              Tactum runs on Linux. There's no Windows or macOS version yet.
            </motion.p>
          )}
          <motion.div className="cta" {...reveal} transition={{ ...reveal.transition, delay: 0.24 }}>
            <a href={release?.url ?? `${RELEASES}/latest`} className={`btn ${onLinux ? 'btn-primary' : 'btn-ghost'}`}>
              Download for Linux
            </a>
            <a href="/account" className="btn btn-ghost">
              Get your free license
            </a>
          </motion.div>
          <motion.p className="caption" {...reveal} transition={{ ...reveal.transition, delay: 0.3 }} aria-live="polite">
            {state === 'loading' && 'Looking up the latest version…'}
            {state === 'none' && 'AppImage for 64-bit Linux'}
            {release &&
              `Version ${release.version} · ${(release.size / 1048576).toFixed(0)} MB · AppImage for 64-bit Linux · ${new Date(
                release.date,
              ).toLocaleDateString(undefined, { day: 'numeric', month: 'short', year: 'numeric' })}`}
          </motion.p>
        </section>

        <section className="section container">
          <p className="label">Install</p>
          <h2>Running in a minute.</h2>
          <ol className="steps">
            <li>
              <span className="step-number">01</span>
              <h3>Make it executable</h3>
              <p>An AppImage is the whole app in one file. Allow it to run, then start it:</p>
              <div className="command">
                <code>{command}</code>
                <button className="btn btn-ghost btn-sm" onClick={copy} aria-live="polite">
                  {copied ? 'Copied' : 'Copy'}
                </button>
              </div>
              <p className="small muted">Or: right-click the file, Properties, and allow running it as a program.</p>
            </li>
            <li>
              <span className="step-number">02</span>
              <h3>Paste your license</h3>
              <p>
                Tactum asks for it at the first start. <a href="/account" className="link">Generate yours</a> with a free account.
              </p>
            </li>
            <li>
              <span className="step-number">03</span>
              <h3>Updating</h3>
              <p>Download the new AppImage and replace the old one. Your license and settings stay.</p>
            </li>
          </ol>
        </section>

        <section className="section container two-col">
          <div>
            <p className="label">System requirements</p>
            <ul className="specs mono">
              <li>64-bit Linux (x86_64) from 2024 or newer: glibc 2.39+, e.g. Ubuntu 24.04, Fedora 40, Debian 13</li>
              <li>FUSE 2 for AppImages (libfuse2), shipped by most distributions</li>
              <li>PipeWire, JACK or ALSA for audio; ALSA for MIDI</li>
              <li>X11 or XWayland for plugin windows</li>
            </ul>
          </div>
          <div>
            <p className="label">More</p>
            <ul className="specs mono">
              <li>
                <a className="link" href={release?.notes ?? RELEASES}>
                  Release notes
                </a>
              </li>
              <li>
                <a className="link" href={RELEASES}>
                  All versions
                </a>
              </li>
              <li>
                <a className="link" href={`https://github.com/${REPO}`}>
                  Source code (GPL-3.0-or-later)
                </a>
              </li>
            </ul>
          </div>
        </section>
      </main>
      <Footer />
    </>
  );
}
