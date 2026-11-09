import { useId } from 'react';
import { profiles } from '../core/profiles/registry';
import type { Device } from './useDevice';

/** What each firmware means for the rest of the app, by profile id. */
const ABOUT: Record<string, string> = {
  'op1/stock': "The OP-1's own firmware (R246). It runs no plugins, so the Plugins tab has nothing to install.",
  'op1/hundekraft': 'hundekraft 3.x and its kennel plugins (.knl), installed over USB with a restart on the OP-1.',
  'op1/opus': 'opus and its .opmod plugins, copied into the mods folder in disk mode.',
  'op1/coop':
    "coop, this project's firmware: stereo tape, two effect and LFO slots, plugin synth engines and effects (.coop). Its plugins are installed, removed and switched on or off from the Plugins tab over USB; each change applies after a power cycle.",
};

interface SettingsProps {
  device: Device;
}

/**
 * The app's settings. The firmware choice is the profile the Library uses (it is the same remembered choice as the
 * Library's profile menu): it decides which plugins the Plugins tab offers and how patches are judged.
 */
export function Settings({ device }: SettingsProps) {
  const name = useId();
  const choices = profiles.filter((p) => p.device === 'op1');
  return (
    <section aria-labelledby="settings-heading">
      <h2 id="settings-heading">Settings</h2>
      <fieldset className="settings-firmware">
        <legend>Your OP-1's firmware</legend>
        <p className="muted">
          Picks the plugins the Plugins tab offers and how patches are checked. The same choice as the profile menu
          on the Library tab; co-op remembers it.
        </p>
        {choices.map((p) => (
          <label key={p.id} className="settings-choice">
            <input
              type="radio"
              name={name}
              value={p.id}
              checked={device.profile.id === p.id}
              disabled={device.busy}
              onChange={() => void device.setProfile(p.id)}
            />
            <span>
              <strong>{p.label}</strong>
              <span className="muted"> {ABOUT[p.id] ?? ''}</span>
            </span>
          </label>
        ))}
      </fieldset>
    </section>
  );
}
