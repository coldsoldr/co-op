/**
 * Plays mono PCM samples (-1..1) at their own sample rate. Returns a function that stops playback.
 * The audio context is closed when playback ends, is stopped, or fails to start. `onEnded` runs
 * once, when playback finishes or is stopped.
 */
export function playPcm(pcm: Float32Array, sampleRate: number, onEnded?: () => void): () => void {
  const context = new AudioContext({ sampleRate });
  let finished = false;
  const finish = (): void => {
    if (finished) return;
    finished = true;
    void context.close().catch(() => {});
    onEnded?.();
  };
  let source: AudioBufferSourceNode;
  try {
    const buffer = context.createBuffer(1, Math.max(1, pcm.length), sampleRate);
    buffer.getChannelData(0).set(pcm);
    source = context.createBufferSource();
    source.buffer = buffer;
    source.connect(context.destination);
    source.onended = finish;
    source.start();
  } catch (error) {
    finished = true;
    void context.close().catch(() => {});
    throw error;
  }
  return () => {
    try {
      source.stop();
    } catch {
      // Already stopped.
    }
    finish();
  };
}
