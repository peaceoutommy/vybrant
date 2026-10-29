import { useEffect, useState, type ReactNode } from 'react';
import { motion } from 'motion/react';
import { Footer, Nav } from './Nav';
import { buildOs, os, osName, type BuildOs } from './platform';

// The release workflow (.github/workflows/release.yml) publishes the AppImage and the Windows installer here on every v* tag
const REPO = 'peaceoutommy/vybrant';
const RELEASES = `https://github.com/${REPO}/releases`;

interface Build {
  size: number;
  url: string;   // the file itself
}
interface Release {
  version: string;
  date: string;
  notes: string;   // the release page
  linux?: Build;
  windows?: Build;
}

// The latest release, from GitHub's API: null while loading or when there is none (the buttons then open the releases page)
function useLatestRelease() {
  const [release, setRelease] = useState<Release | null>(null);
  const [state, setState] = useState<'loading' | 'ready' | 'none'>('loading');
  useEffect(() => {
    fetch(`https://api.github.com/repos/${REPO}/releases/latest`)
      .then((r) => (r.ok ? r.json() : Promise.reject()))
      .then((r) => {
        const build = (name: RegExp): Build | undefined => {   // the builds we make; never another arch
          const a = r.assets.find((a: { name: string }) => name.test(a.name));
          return a && { size: a.size, url: a.browser_download_url };
        };
        const linux = build(/x86_64\.AppImage$/), windows = build(/windows-x64-setup\.exe$/) ?? build(/windows-x64\.zip$/);
        if (!linux && !windows) return setState('none');
        setRelease({ version: r.tag_name.replace(/^v/, ''), date: r.published_at, notes: r.html_url, linux, windows });
        setState('ready');
      })
      .catch(() => setState('none'));
  }, []);
  return { release, state };
}

const kind: Record<BuildOs, string> = { linux: 'AppImage for 64-bit Linux', windows: 'Installer for 64-bit Windows' };
const other: BuildOs = buildOs === 'linux' ? 'windows' : 'linux';
const notice: Partial<Record<typeof os, string>> = {
  mac: "Vybrant runs on Windows and Linux. There's no macOS version yet.",
  mobile: 'Vybrant is a desktop app for Windows and Linux: download it on your computer.',
  other: 'Vybrant runs on Windows and Linux.',
};
const requirements: Record<BuildOs, string[]> = {
  windows: ['Windows 10 or 11, 64-bit', 'Any audio device (WASAPI, DirectSound or MME)', 'VST3, VST2 and CLAP plugins (64-bit)'],
  linux: [
    'Any 64-bit Linux (x86_64) distribution from 2024 on: Arch, CachyOS, Manjaro, Ubuntu 24.04+, Fedora 40+, Debian 13+ (glibc 2.39 or newer)',
    'FUSE 2 for AppImages (libfuse2), shipped by most distributions',
    'PipeWire, JACK or ALSA for audio; ALSA for MIDI',
    'X11 or XWayland for plugin windows',
    'VST3, VST2, CLAP and LV2 plugins (64-bit)',
  ],
};

const reveal = {
  initial: { opacity: 0, y: 20 },
  animate: { opacity: 1, y: 0 },
  transition: { duration: 0.7, ease: [0.22, 1, 0.36, 1] },
} as const;

// A small drawing of the screen a step shows, in the site's colours (drawn, not a screenshot)
function Screen({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className="screen" aria-hidden>
      <div className="screen-bar">
        <span>{title}</span>
        <i />
      </div>
      <div className="screen-body">{children}</div>
    </div>
  );
}

// The last step on both: the dialog Vybrant opens until it has a license
function LicenseStep() {
  return (
    <li>
      <Screen title="Vybrant license">
        <span className="screen-field">eyJ2IjoxLC...</span>
        <span className="screen-btn">Get a license</span>
        <span className="screen-btn on">Activate</span>
      </Screen>
      <span className="step-number">03</span>
      <h3>Paste your license</h3>
      <p>
        Vybrant asks for it once. <a href="/account" className="link">Generate yours</a> with a free account.
      </p>
    </li>
  );
}

function WindowsSteps() {
  return (
    <ol className="steps">
      <li>
        <Screen title="Vybrant Setup">
          <p className="screen-text">Select Setup Install Mode</p>
          <span className="screen-radio on">Install for me only (recommended)</span>
          <span className="screen-radio">Install for all users</span>
          <span className="screen-btn on">Next</span>
        </Screen>
        <span className="step-number">01</span>
        <h3>Run the installer</h3>
        <p>Double-click the file you downloaded and choose Install for me only: no admin rights needed. Then click Next through the pages.</p>
      </li>
      <li>
        <Screen title="Vybrant Setup">
          <p className="screen-text">Completing the Vybrant Setup Wizard</p>
          <span className="screen-check on">Launch Vybrant</span>
          <span className="screen-btn on">Finish</span>
        </Screen>
        <span className="step-number">02</span>
        <h3>Finish and launch</h3>
        <p>Vybrant is in the Start menu from now on, and .mtp projects open in it. Leave Launch Vybrant ticked and click Finish.</p>
      </li>
      <LicenseStep />
    </ol>
  );
}

function LinuxSteps() {
  const [copied, setCopied] = useState(false);
  const command = 'chmod +x Vybrant-*.AppImage && ./Vybrant-*.AppImage';
  const copy = async () => {
    await navigator.clipboard.writeText(command);
    setCopied(true);
    setTimeout(() => setCopied(false), 1800);
  };
  return (
    <ol className="steps">
      <li>
        <Screen title="Terminal">
          <p className="screen-mono">$ chmod +x Vybrant-*.AppImage</p>
          <p className="screen-mono">$ ./Vybrant-*.AppImage</p>
        </Screen>
        <span className="step-number">01</span>
        <h3>Make it executable</h3>
        <p>An AppImage is the whole app in one file: allow it to run, then start it. Or right-click it, Properties, and allow running it as a program.</p>
        <button className="btn btn-ghost btn-sm copy" onClick={copy} aria-live="polite">
          {copied ? 'Copied' : 'Copy the command'}
        </button>
      </li>
      <li>
        <Screen title="Install Vybrant">
          <p className="screen-text">Add Vybrant to your applications menu?</p>
          <span className="screen-btn on">Install</span>
          <span className="screen-btn">Not now</span>
        </Screen>
        <span className="step-number">02</span>
        <h3>Add it to your apps</h3>
        <p>On its first start it offers to install itself: a launcher in your applications menu, and .mtp projects open in it.</p>
      </li>
      <LicenseStep />
    </ol>
  );
}

export function Download() {
  const { release, state } = useLatestRelease();
  const [shown, setShown] = useState<BuildOs>(buildOs);   // whose instructions: the visitor's own unless they switch
  const mine = release?.[buildOs];

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
            Get <em>Vybrant.</em>
          </motion.h1>
          <motion.p className="lead" {...reveal} transition={{ ...reveal.transition, delay: 0.16 }}>
            One download for every update. Vybrant asks for your license the first time it starts, and it's free with an
            account.
          </motion.p>
          {notice[os] && (
            <motion.p className="notice" {...reveal} transition={{ ...reveal.transition, delay: 0.2 }}>
              {notice[os]}
            </motion.p>
          )}
          <motion.div className="cta" {...reveal} transition={{ ...reveal.transition, delay: 0.24 }}>
            <a href={mine?.url ?? `${RELEASES}/latest`} className="btn btn-primary">
              Download for {osName[buildOs]}
            </a>
            <a href="/account" className="btn btn-ghost">
              Get your free license
            </a>
          </motion.div>
          <motion.p className="caption" {...reveal} transition={{ ...reveal.transition, delay: 0.3 }} aria-live="polite">
            {state === 'loading' && 'Looking up the latest version…'}
            {(state === 'none' || (release && !release[buildOs])) && kind[buildOs]}
            {release && mine &&
              `Version ${release.version} · ${(mine.size / 1048576).toFixed(0)} MB · ${kind[buildOs]} · ${new Date(
                release.date,
              ).toLocaleDateString(undefined, { day: 'numeric', month: 'short', year: 'numeric' })}`}
            <br />
            <a href={release?.[other]?.url ?? `${RELEASES}/latest`} className="link">
              Also for {osName[other]}
            </a>
          </motion.p>
        </section>

        <section className="section container">
          <div className="section-head">
            <div>
              <p className="label">Install</p>
              <h2>Running in a minute.</h2>
            </div>
            <div className="os-switch" role="group" aria-label="Instructions for">
              {(['windows', 'linux'] as BuildOs[]).map((o) => (
                <button key={o} className={shown === o ? 'on' : undefined} aria-pressed={shown === o} onClick={() => setShown(o)}>
                  {osName[o]}
                </button>
              ))}
            </div>
          </div>
          {shown === 'windows' ? <WindowsSteps /> : <LinuxSteps />}
          <p className="small muted update-note">
            Updating: {shown === 'windows' ? 'run the new installer, it replaces the old version.' : 'start the new AppImage, it offers to replace the installed one.'}{' '}
            Your license, settings and projects stay.
          </p>
        </section>

        <section className="section container two-col">
          <div>
            <p className="label">System requirements · {osName[shown]}</p>
            <ul className="specs mono">
              {requirements[shown].map((r) => (
                <li key={r}>{r}</li>
              ))}
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
