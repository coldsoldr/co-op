import { useEffect, useRef } from 'react';
import guide from '../../../docs/plugin-release-guide.md?raw';
import { Markdown } from './Markdown';

/** The release guide for plugin authors (docs/plugin-release-guide.md), rendered like a manual. */
export function ReleaseGuide({ onClose }: { onClose: () => void }) {
  const heading = useRef<HTMLHeadingElement>(null);
  useEffect(() => {
    window.scrollTo(0, 0);
    heading.current?.focus();
  }, []);
  // The document's own title becomes the page heading; the rest renders under it.
  const body = guide.replace(/^# .*\n+/, '');
  return (
    <section aria-labelledby="release-guide-heading" className="release-guide">
      <div className="packs-header">
        <h2 id="release-guide-heading" ref={heading} tabIndex={-1}>
          Release guide for plugin authors
        </h2>
        <button type="button" onClick={onClose}>
          Back
        </button>
      </div>
      <Markdown source={body} folderUrl={import.meta.env.BASE_URL} headingOffset={1} />
    </section>
  );
}
