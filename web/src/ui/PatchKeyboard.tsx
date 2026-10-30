import { useEffect, useRef } from 'react';
import { KEY_COUNT } from '../core/codec/preview';
import type { KeyboardPlayer } from './keyboard-player';

/** The lowest key is an F, like the OP-1's. These are the black keys counted from it. */
const SHARP_OFFSETS = new Set([1, 3, 5, 8, 10]);
const isSharp = (key: number): boolean => SHARP_OFFSETS.has(key % 12);
/** A piano-style row of the computer keyboard, starting on the OP-1's middle C. */
const COMPUTER_KEYS = 'awsedftgyhujkolp';
const FIRST_COMPUTER_KEY = 7;

function keyOf(event: KeyboardEvent): number | null {
  const index = COMPUTER_KEYS.indexOf(event.key.toLowerCase());
  return index < 0 || event.ctrlKey || event.metaKey || event.altKey ? null : FIRST_COMPUTER_KEY + index;
}

const isTyping = (target: EventTarget | null): boolean =>
  target instanceof HTMLElement && (target.isContentEditable || ['INPUT', 'SELECT', 'TEXTAREA'].includes(target.tagName));

/** 24 keys for the chosen patch, playable with the pointer or the computer keyboard. */
export function PatchKeyboard({ name, player, kit, onClose }: { name: string; player: KeyboardPlayer; kit: boolean; onClose: () => void }) {
  const pressed = useRef(new Set<number>());

  useEffect(() => {
    const down = (event: KeyboardEvent): void => {
      if (event.repeat || isTyping(event.target)) return;
      const key = keyOf(event);
      if (key === null) return;
      event.preventDefault();
      player.press(key);
    };
    const up = (event: KeyboardEvent): void => {
      const key = keyOf(event);
      if (key !== null) player.release(key);
    };
    window.addEventListener('keydown', down);
    window.addEventListener('keyup', up);
    return () => {
      window.removeEventListener('keydown', down);
      window.removeEventListener('keyup', up);
    };
  }, [player]);

  const stop = (key: number): void => {
    pressed.current.delete(key);
    player.release(key);
  };

  return (
    <section className="patch-keyboard" aria-label={`Keyboard for ${name}`}>
      <div className="patch-keyboard-head">
        <div>
          <strong>{name}</strong>
          <span className="muted">
            {kit ? ' Each key plays one slice of the kit.' : ' Plays the stored note repitched, so envelopes and LFOs change speed with it.'}
          </span>
        </div>
        <button type="button" onClick={onClose}>
          Close keyboard
        </button>
      </div>
      <div className="keys-row">
        {Array.from({ length: KEY_COUNT }, (_, key) => (
          <button
            key={key}
            type="button"
            className={isSharp(key) ? 'piano-key sharp' : 'piano-key'}
            aria-label={`Key ${key + 1}`}
            onPointerDown={() => {
              pressed.current.add(key);
              player.press(key);
            }}
            onPointerUp={() => stop(key)}
            onPointerLeave={() => pressed.current.has(key) && stop(key)}
            onKeyDown={(e) => {
              if ((e.key === 'Enter' || e.key === ' ') && !e.repeat) {
                e.preventDefault();
                player.press(key);
              }
            }}
            onKeyUp={(e) => {
              if (e.key === 'Enter' || e.key === ' ') player.release(key);
            }}
          />
        ))}
      </div>
    </section>
  );
}
