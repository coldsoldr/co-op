/**
 * Plays mono PCM samples (-1..1) at their own sample rate. Returns a function that stops playback;
 * the audio context is closed when playback ends or is stopped. onEnded runs when playback finishes
 * or is stopped.
 */
export function playPcm(pcm: Float32Array, sampleRate: number, onEnded?: () => void): () => void {
  const context = new AudioContext({ sampleRate });
  const buffer = context.createBuffer(1, Math.max(1, pcm.length), sampleRate);
  buffer.getChannelData(0).set(pcm);
  const source = context.createBufferSource();
  source.buffer = buffer;
  source.connect(context.destination);
  let closed = false;
  const close = (): void => {
    if (closed) return;
    closed = true;
    void context.close().catch(() => {});
  };
  source.onended = () => {
    close();
    onEnded?.();
  };
  source.start();
  return () => {
    try {
      source.stop();
    } catch {
      // Already stopped.
    }
    close();
  };
}
