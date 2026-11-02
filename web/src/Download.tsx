import { useState, type ReactNode } from 'react';
import { motion } from 'motion/react';
import { Footer } from './Nav';
import { buildOs, os, osName, type BuildOs } from './platform';
import { useLatestRelease, type Build } from './release';
import { Intro } from './Intro';

const kind: Record<BuildOs, string> = { linux: 'AppImage · 64-bit Linux', windows: 'Installer · 64-bit Windows 10 and 11' };
const other: BuildOs = buildOs === 'linux' ? 'windows' : 'linux';
const notice: Partial<Record<typeof os, string>> = {
  mac: "Vybrant runs on Windows and Linux. There's no macOS version yet.",
  mobile: 'Vybrant is a desktop app for Windows and Linux: download it on your computer.',
  other: 'Vybrant runs on Windows and Linux.',
};
const requirements: Record<BuildOs, [string, string][]> = {
  windows: [
    ['System', 'Windows 10 or 11, 64-bit'],
    ['Audio', 'Any audio device: WASAPI, DirectSound or MME'],
    ['Plugins', 'VST3, VST2 and CLAP, 64-bit'],
  ],
  linux: [
    ['System', 'Any 64-bit (x86_64) distribution from 2024 on: Arch, CachyOS, Manjaro, Ubuntu 24.04+, Fedora 40+, Debian 13+ (glibc 2.39 or newer)'],
    ['AppImage', 'FUSE 2 (libfuse2), shipped by most distributions'],
    ['Audio', 'PipeWire, JACK or ALSA; ALSA for MIDI'],
    ['Plugin windows', 'X11 or XWayland'],
    ['Plugins', 'VST3, VST2, CLAP and LV2, 64-bit'],
  ],
};
const day = (iso: string) => new Date(iso).toLocaleDateString(undefined, { day: 'numeric', month: 'short', year: 'numeric' });

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

// One system's download: the visitor's own comes first and leads
function BuildCard({ system, build, version, mine, delay }: { system: BuildOs; build?: Build; version?: string; mine: boolean; delay: number }) {
  return (
    <motion.article className={mine ? 'build mine' : 'build'} {...reveal} transition={{ ...reveal.transition, delay }}>
      <div className="build-head">
        <h2>{osName[system]}</h2>
        {/* only on that system itself: on a phone or a Mac, Windows merely leads */}
        {mine && os === system && <span className="build-tag">Your system</span>}
      </div>
      <p className="build-kind">{kind[system]}</p>
      <dl className="build-facts">
        <div>
          <dt>Version</dt>
          <dd>{version ?? '—'}</dd>
        </div>
        <div>
          <dt>Size</dt>
          <dd>{build ? `${(build.size / 1048576).toFixed(0)} MB` : '—'}</dd>
        </div>
      </dl>
      <a href={build?.url} className={mine ? 'btn btn-primary' : 'btn btn-ghost'} aria-disabled={!build}>
        Download for {osName[system]}
      </a>
    </motion.article>
  );
}

export function Download() {
  const { release, state } = useLatestRelease();
  const [shown, setShown] = useState<BuildOs>(buildOs);   // whose instructions: the visitor's own unless they switch

  return (
    <>
      <div className="glow" aria-hidden />
      <main>
        <section className="hero container">
          <motion.p className="label" {...reveal}>
            Download
          </motion.p>
          <motion.h1 {...reveal} transition={{ ...reveal.transition, delay: 0.08 }}>
            Get <em>Vybrant.</em>
          </motion.h1>
          <motion.p className="lead" {...reveal} transition={{ ...reveal.transition, delay: 0.16 }}>
            One download for every update. Vybrant asks for your license the first time it starts, and it's free with an{' '}
            <a href="/account" className="link">
              account
            </a>
            .
          </motion.p>
          {notice[os] && (
            <motion.p className="notice" {...reveal} transition={{ ...reveal.transition, delay: 0.2 }}>
              {notice[os]}
            </motion.p>
          )}
        </section>

        <section className="container builds" aria-label="Downloads">
          {[buildOs, other].map((o, i) => (
            <BuildCard key={o} system={o} build={release?.[o]} version={release?.version} mine={o === buildOs} delay={0.24 + i * 0.08} />
          ))}
        </section>
        <motion.p className="container release-meta" {...reveal} transition={{ ...reveal.transition, delay: 0.36 }} aria-live="polite">
          {state === 'loading' && 'Looking up the latest version…'}
          {state === 'none' && 'Downloads are unavailable right now, try again in a minute.'}
          {release && `Version ${release.version}, released ${day(release.date)}. Your license, settings and projects carry over to every update.`}
        </motion.p>

        {!!release?.notes.length && (
          <section className="section container">
            <Intro label="What's new" title={`In version ${release.version}.`} text="Vybrant offers each update itself; this is what the latest one brings." />
            <ul className="notes">
              {release.notes.map((n) => (
                <li key={n}>{n}</li>
              ))}
            </ul>
          </section>
        )}

        <section className="section container">
          <Intro label="Install" title="Running in a minute.">
            <div className="os-switch" role="group" aria-label="Instructions for">
              {(['windows', 'linux'] as BuildOs[]).map((o) => (
                <button key={o} className={shown === o ? 'on' : undefined} aria-pressed={shown === o} onClick={() => setShown(o)}>
                  {osName[o]}
                </button>
              ))}
            </div>
          </Intro>
          {shown === 'windows' ? <WindowsSteps /> : <LinuxSteps />}
          <p className="small muted update-note">
            Updating: {shown === 'windows' ? 'run the new installer, it replaces the old version.' : 'start the new AppImage, it offers to replace the installed one.'}{' '}
            Your license, settings and projects stay.
          </p>
        </section>

        <section className="section container">
          <Intro label="Requirements" title={`What it needs on ${osName[shown]}.`} text="A recent 64-bit system and an audio device. Plugins are optional." />
          <dl className="reqs">
            {requirements[shown].map(([k, v]) => (
              <div key={k}>
                <dt>{k}</dt>
                <dd>{v}</dd>
              </div>
            ))}
          </dl>
        </section>
      </main>
      <Footer />
    </>
  );
}
