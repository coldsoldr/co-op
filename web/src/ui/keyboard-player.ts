import type { KeyPlan } from '../core/codec/preview';

/** Long enough to avoid a click when a key is released. */
const RELEASE_SECONDS = 0.02;

/** Plays one patch's audio from the on-screen keys. Close it to free its audio context. */
export class KeyboardPlayer {
  private readonly context: AudioContext;
  private readonly buffer: AudioBuffer;
  private readonly held = new Map<number, { source: AudioBufferSourceNode; gain: GainNode }>();
  private whole: AudioBufferSourceNode | null = null;

  constructor(
    pcm: Float32Array,
    sampleRate: number,
    private readonly plan: KeyPlan,
  ) {
    this.context = new AudioContext({ sampleRate });
    this.buffer = this.context.createBuffer(1, Math.max(1, pcm.length), sampleRate);
    this.buffer.getChannelData(0).set(pcm);
  }

  private start(key: number): { source: AudioBufferSourceNode; gain: GainNode } {
    void this.context.resume();
    const source = this.context.createBufferSource();
    const gain = this.context.createGain();
    source.buffer = this.buffer;
    source.playbackRate.value = this.plan.rate(key);
    source.connect(gain).connect(this.context.destination);
    const offset = this.plan.startFrame(key) / this.buffer.sampleRate;
    const end = this.plan.endFrame(key);
    if (end === null) source.start(0, offset);
    else source.start(0, offset, Math.max(0, (end - this.plan.startFrame(key)) / this.buffer.sampleRate));
    return { source, gain };
  }

  press(key: number): void {
    if (this.held.has(key)) return;
    this.held.set(key, this.start(key));
  }

  release(key: number): void {
    const voice = this.held.get(key);
    if (!voice) return;
    this.held.delete(key);
    const now = this.context.currentTime;
    voice.gain.gain.setTargetAtTime(0, now, RELEASE_SECONDS / 3);
    voice.source.stop(now + RELEASE_SECONDS);
  }

  /** Plays all the audio as recorded; `onEnded` runs when it finishes or is stopped. */
  playWhole(onEnded: () => void): void {
    this.stopWhole();
    void this.context.resume();
    const source = this.context.createBufferSource();
    source.buffer = this.buffer;
    source.connect(this.context.destination);
    source.onended = () => {
      if (this.whole === source) this.whole = null;
      onEnded();
    };
    source.start();
    this.whole = source;
  }

  stopWhole(): void {
    try {
      this.whole?.stop();
    } catch {
      // Already stopped.
    }
    this.whole = null;
  }

  close(): void {
    void this.context.close().catch(() => {});
  }
}
