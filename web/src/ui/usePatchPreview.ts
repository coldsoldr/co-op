import { useCallback, useEffect, useRef, useState } from 'react';
import { decodePcm, readPatch } from '../core/codec/patch';
import { hasPreview, keyPlan } from '../core/codec/preview';
import { messageOf } from '../core/util';
import { KeyboardPlayer } from './keyboard-player';

const NO_PREVIEW = 'This patch holds no preview: the OP-1 only stored its voice clip here.';

export interface OpenPreview {
  id: string;
  name: string;
  player: KeyboardPlayer;
  kit: boolean;
}

/** A patch's stored audio in the browser, with its keyboard. `active` is false while the tab is hidden. */
export function usePatchPreview(active: boolean) {
  const [open, setOpen] = useState<OpenPreview | null>(null);
  const [playing, setPlaying] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const openRef = useRef<OpenPreview | null>(null);
  /** Bumped by every click, so a read that finishes late never starts. */
  const token = useRef(0);

  const close = useCallback(() => {
    token.current++;
    openRef.current?.player.stopWhole();
    openRef.current?.player.close();
    openRef.current = null;
    setOpen(null);
    setPlaying(null);
  }, []);

  // Hiding the tab only stops the sound: the keyboard is not shown and the patch stays open.
  useEffect(() => {
    if (!active) openRef.current?.player.stopWhole();
  }, [active]);
  useEffect(
    () => () => {
      openRef.current?.player.close();
    },
    [],
  );

  const toggle = useCallback(
    async (id: string, name: string, load: () => Promise<Uint8Array>) => {
      const was = openRef.current?.id === id && playing === id;
      const mine = ++token.current;
      setError(null);
      openRef.current?.player.stopWhole();
      setPlaying(null);
      if (was) return;
      try {
        let current = openRef.current?.id === id ? openRef.current : null;
        if (!current) {
          const patch = readPatch(await load());
          if (!(await hasPreview(patch))) throw new Error(NO_PREVIEW);
          if (mine !== token.current) return;
          close();
          token.current = mine;
          const player = new KeyboardPlayer(decodePcm(patch), patch.comm.sampleRate, keyPlan(patch));
          current = { id, name, player, kit: patch.meta?.type === 'drum' };
          openRef.current = current;
          setOpen(current);
        }
        setPlaying(id);
        current.player.playWhole(() => setPlaying((now) => (now === id ? null : now)));
      } catch (e) {
        if (mine === token.current) setError(`${name}: ${messageOf(e)}`);
      }
    },
    [close, playing],
  );

  return { open, playing, error, toggle, close };
}
