import { useEffect, useRef, useState } from 'react';
import { PLUGIN_PROMPT } from '../core/plugins/submission';

const REPO = 'https://github.com/coldsoldr/co-op';
const GUIDE_URL = `${REPO}/blob/main/docs/contributing-plugins.md`;
const EXAMPLE_URL = `${REPO}/tree/main/plugins/bitcrusher`;
const REQUEST_URL = `${REPO}/issues/new?template=plugin-request.yml`;

/** How to get a plugin into the library, with the prompt that prepares it for an author's agent. */
export function PluginSubmit({ onBack }: { onBack: () => void }) {
  const [copied, setCopied] = useState<'yes' | 'failed' | null>(null);
  const back = useRef<HTMLButtonElement>(null);

  useEffect(() => back.current?.focus(), []);

  function copy(): void {
    navigator.clipboard.writeText(PLUGIN_PROMPT).then(
      () => setCopied('yes'),
      () => setCopied('failed'),
    );
  }

  return (
    <div className="plugin-submit">
      <p>
        <button ref={back} type="button" onClick={onBack}>
          Back to plugins
        </button>
      </p>
      <p>
        Plugins join the library by pull request on GitHub. A plugin runs as firmware code on the OP-1, so a maintainer
        decides each one: only from an author they trust, or after looking at its source.
      </p>

      <h3>What your plugin needs</h3>
      <p>One folder named after the plugin’s id, with exactly four files:</p>
      <ul className="file-list">
        <li>
          <code>plugin.json</code>: its name, author, kind, version, licence, description and a link to the source
        </li>
        <li>
          <code>&lt;id&gt;.knl</code>: the built plugin (<code>.op1p</code> for op1r, <code>.opmod</code> for opus)
        </li>
        <li>
          <code>manual.md</code>: the manual, with the same sections as every other plugin
        </li>
        <li>
          <code>screenshot.png</code>: its screen on black, 960 by 480 pixels
        </li>
      </ul>
      <p>
        The <a href={EXAMPLE_URL}>bitcrusher folder</a> is a complete example.
      </p>

      <h3>Let your coding agent prepare it</h3>
      <p>
        Paste this into your coding agent from the folder that holds the plugin’s source. It prepares the folder so your
        plugin looks like the rest of the library. Check what it writes against how the plugin really behaves.
      </p>
      <p>
        <button type="button" className="primary" onClick={copy}>
          Copy the prompt
        </button>{' '}
        <span role="status">
          {copied === 'yes' && 'Copied.'}
          {copied === 'failed' && 'Could not copy. Select the text below instead.'}
        </span>
      </p>
      <pre className="prompt" tabIndex={0} aria-label="The prompt for your coding agent">
        {PLUGIN_PROMPT}
      </pre>

      <h3>Send it in</h3>
      <ol>
        <li>
          Fork <a href={REPO}>coldsoldr/co-op</a> on GitHub.
        </li>
        <li>
          In your fork, open the <code>plugins</code> folder, choose Add file, then Upload files, and drag your whole
          folder onto the page. Commit it to a new branch.
        </li>
        <li>Open a pull request and fill in the checklist, including how you built the file.</li>
        <li>A check runs on the pull request and says if anything needs fixing.</li>
      </ol>
      <p>
        <a href={GUIDE_URL}>Read the full guide</a> or, if you would rather not use a pull request,{' '}
        <a href={REQUEST_URL}>offer your plugin in an issue</a>.
      </p>
    </div>
  );
}
