import { useEffect, useRef } from 'react';
import { RELEASES } from '../core/releases';
import { Markdown } from './Markdown';

/** Every release, newest first and open. The notes are rendered by the same safe parser as manuals. */
export function Changelog({ onClose }: { onClose: () => void }) {
  const heading = useRef<HTMLHeadingElement>(null);
  useEffect(() => {
    window.scrollTo(0, 0);
    heading.current?.focus();
  }, []);
  return (
    <section aria-labelledby="changelog-heading">
      <div className="packs-header">
        <h2 id="changelog-heading" ref={heading} tabIndex={-1}>
          Changelog
        </h2>
        <button type="button" onClick={onClose}>
          Back
        </button>
      </div>
      {RELEASES.map((release, i) => (
        <details key={release.version} className="release" open={i === 0}>
          <summary>
            <h3>{release.title}</h3>
            {release.date && <span className="mono muted">{release.date}</span>}
          </summary>
          <Markdown source={release.body} folderUrl={import.meta.env.BASE_URL} headingOffset={2} />
        </details>
      ))}
    </section>
  );
}
