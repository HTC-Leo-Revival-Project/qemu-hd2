// AudioWorklet: plays interleaved S16LE stereo PCM pushed from the main thread.
// Keeps about 60 ms queued; if the network delivers a burst, drops the oldest
// audio to hold latency below 250 ms.
class PcmPlayer extends AudioWorkletProcessor {
  constructor() {
    super();
    this.size = 48000; // 1 s ring per channel
    this.l = new Float32Array(this.size);
    this.r = new Float32Array(this.size);
    this.rd = 0;
    this.wr = 0;
    this.primed = false;
    this.port.onmessage = (e) => this.push(new Int16Array(e.data));
  }
  get avail() {
    return (this.wr - this.rd + this.size) % this.size;
  }
  push(s) {
    for (let i = 0; i + 1 < s.length; i += 2) {
      this.l[this.wr] = s[i] / 32768;
      this.r[this.wr] = s[i + 1] / 32768;
      this.wr = (this.wr + 1) % this.size;
      if (this.wr === this.rd) this.rd = (this.rd + 1) % this.size;
    }
    if (this.avail > 12000) this.rd = (this.wr - 2880 + this.size) % this.size;
  }
  process(_inputs, outputs) {
    const out = outputs[0];
    const n = out[0].length;
    if (!this.primed && this.avail >= 2880) this.primed = true;
    for (let i = 0; i < n; i++) {
      if (this.primed && this.avail > 0) {
        out[0][i] = this.l[this.rd];
        out[1][i] = this.r[this.rd];
        this.rd = (this.rd + 1) % this.size;
      } else {
        out[0][i] = 0;
        out[1][i] = 0;
        this.primed = false;
      }
    }
    return true;
  }
}
registerProcessor("pcm-player", PcmPlayer);
