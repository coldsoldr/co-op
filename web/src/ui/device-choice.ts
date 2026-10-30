import { useCallback, useState } from 'react';

export type SiteDevice = 'op1' | 'opxy';

const DEVICE_KEY = 'co-op.device';

/** The devices the logo menu lists; one without an id cannot be chosen yet. */
export const DEVICES: ReadonlyArray<{ id: SiteDevice | null; label: string }> = [
  { id: 'op1', label: 'OP-1' },
  { id: 'opxy', label: 'OP-XY' },
  { id: null, label: 'OP-1F (coming soon)' },
];

function storedDevice(): SiteDevice {
  try {
    return localStorage.getItem(DEVICE_KEY) === 'opxy' ? 'opxy' : 'op1';
  } catch {
    return 'op1';
  }
}

const applyDevice = (device: SiteDevice): void => {
  document.documentElement.dataset.device = device;
};

/** Sets the device on the page, so it paints right the first time. Called before React renders. */
export function initDevice(): void {
  applyDevice(storedDevice());
}

/** The device the site is showing, and a setter that remembers the choice. */
export function useSiteDevice(): { device: SiteDevice; choose: (next: SiteDevice) => void } {
  const [device, setDevice] = useState<SiteDevice>(storedDevice);
  const choose = useCallback((next: SiteDevice) => {
    setDevice(next);
    applyDevice(next);
    try {
      localStorage.setItem(DEVICE_KEY, next);
    } catch {
      // Without storage the choice lasts until the page closes.
    }
  }, []);
  return { device, choose };
}
