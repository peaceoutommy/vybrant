import { useEffect } from 'react';
import { motion } from 'motion/react';
import { Footer } from './Nav';
import { groups, useLatestRelease, useReleases, type Entry } from './release';

const day = (iso: string) => new Date(iso).toLocaleDateString(undefined, { day: 'numeric', month: 'long', year: 'numeric' });
const reveal = { initial: { opacity: 0, y: 20 }, animate: { opacity: 1, y: 0 }, transition: { duration: 0.7, ease: [0.22, 1, 0.36, 1] } } as const;

function Release({ entry, latest }: { entry: Entry; latest: boolean }) {
  const parts = groups(entry.notes);
  return (
    <article className="release" id={`v${entry.version}`}>
      <div className="release-side">
        <a href={`#v${entry.version}`} className="release-version">
          {entry.version}
        </a>
        {latest && <span className="build-tag">Latest</span>}
        <time dateTime={entry.date}>{day(entry.date)}</time>
      </div>
      <div className="release-body">
        {parts.length ? (
          parts.map((g, i) => (
            <div key={i} className="release-group">
              {g.title && <h3 className="label">{g.title}</h3>}
              <ul className="notes">
                {g.items.map((item) => (
                  <li key={item}>{item}</li>
                ))}
              </ul>
            </div>
          ))
        ) : (
          <p className="muted">No notes were written for this release.</p>
        )}
      </div>
    </article>
  );
}

// Every release, newest first: the version and date pinned at the left while its notes scroll by
export function Changelog() {
  const { entries, failed } = useReleases();
  const { release } = useLatestRelease();
  // before the first release that publishes releases.json, the latest one alone
  const list = entries ?? (failed && release ? [{ version: release.version, date: release.date, notes: release.notes.map((n) => `- ${n}`).join('\n') }] : null);
  useEffect(() => {
    if (list && location.hash) document.getElementById(location.hash.slice(1))?.scrollIntoView();   // a link to one release
  }, [!!list]);

  return (
    <>
      <div className="glow" aria-hidden />
      <main>
        <section className="hero container">
          <motion.p className="label" {...reveal}>
            Changelog
          </motion.p>
          <motion.h1 {...reveal} transition={{ ...reveal.transition, delay: 0.08 }}>
            What's <em>new.</em>
          </motion.h1>
          <motion.p className="lead" {...reveal} transition={{ ...reveal.transition, delay: 0.16 }}>
            Every release of Vybrant, newest first. The app offers each update itself, and your projects and settings carry over.
          </motion.p>
        </section>
        <section className="container changelog" aria-live="polite">
          {list ? list.map((e, i) => <Release key={e.version} entry={e} latest={i === 0} />) : (
            <p className="muted release-wait">{failed ? 'The changelog is unavailable right now, try again in a minute.' : 'Loading releases…'}</p>
          )}
        </section>
      </main>
      <Footer />
    </>
  );
}
