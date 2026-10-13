import type { WebSocket } from "ws";
import { H264Encoder, JpegEncoder, type H264Chunk, type H264Config } from "./encoder.ts";
import type { EmulatorConfig } from "../lib/emulator-types.ts";

/*
 * Fan-out of the emulated screen to browsers.  One encoder per format is
 * shared by every viewer; a viewer that joins restarts the H.264 encoder so
 * it starts on a keyframe.
 *
 * Video socket messages (binary): byte 0 = kind
 *   1 config: JSON { codec, width, height } then 0x00 then the avcC record
 *   2 key frame, 3 delta frame (AVCC length-prefixed NAL units)
 *   4 JPEG picture
 */

type Viewer = { ws: WebSocket; format: "h264" | "jpeg"; synced: boolean };

const MSG_CONFIG = 1, MSG_KEY = 2, MSG_DELTA = 3, MSG_JPEG = 4;

export class StreamHub {
  private viewers = new Set<Viewer>();
  private audioClients = new Set<WebSocket>();
  private h264: H264Encoder | null = null;
  private jpeg: JpegEncoder | null = null;
  private frame: Buffer | null = null;
  private width = 480;
  private height = 800;
  private lastPush = 0;
  private timer: NodeJS.Timeout | null = null;
  private opts: Pick<EmulatorConfig, "encoder" | "quality" | "maxFps">;
  private stats = { frames: 0, bytes: 0, fps: 0, kbps: 0, t: Date.now() };

  constructor(opts: Pick<EmulatorConfig, "encoder" | "quality" | "maxFps">) {
    this.opts = opts;
  }

  setOptions(opts: Pick<EmulatorConfig, "encoder" | "quality" | "maxFps">) {
    const changed = opts.encoder !== this.opts.encoder || opts.quality !== this.opts.quality || opts.maxFps !== this.opts.maxFps;
    this.opts = opts;
    if (changed && this.h264) this.restartH264();
  }

  get clientCount() {
    return this.viewers.size;
  }

  get statsSnapshot() {
    const now = Date.now();
    const dt = (now - this.stats.t) / 1000;
    if (dt >= 1) {
      this.stats.fps = Math.round(this.stats.frames / dt);
      this.stats.kbps = Math.round((this.stats.bytes * 8) / dt / 1000);
      this.stats.frames = 0;
      this.stats.bytes = 0;
      this.stats.t = now;
    }
    return { fps: this.stats.fps, kbps: this.stats.kbps, clients: this.viewers.size, width: this.width, height: this.height };
  }

  /** A complete screen from the emulator (BGRX). */
  onFrame(fb: Buffer, w: number, h: number) {
    if (w !== this.width || h !== this.height) {
      this.width = w;
      this.height = h;
      this.frame = null;
      if (this.h264) this.restartH264();
      if (this.jpeg) this.restartJpeg();
    }
    this.frame = fb;
    const minGap = 1000 / this.opts.maxFps;
    const since = Date.now() - this.lastPush;
    if (since >= minGap) this.pushFrame();
    else if (!this.timer) this.timer = setTimeout(() => this.pushFrame(), minGap - since);
  }

  private pushFrame() {
    if (this.timer) clearTimeout(this.timer);
    this.timer = null;
    if (!this.frame) return;
    this.lastPush = Date.now();
    this.h264?.push(this.frame);
    this.jpeg?.push(this.frame);
  }

  get lastFrame() {
    return this.frame ? { fb: this.frame, w: this.width, h: this.height } : null;
  }

  addViewer(ws: WebSocket, format: "h264" | "jpeg") {
    const v: Viewer = { ws, format, synced: false };
    this.viewers.add(v);
    ws.on("close", () => {
      this.viewers.delete(v);
      this.reapEncoders();
    });
    if (format === "h264") this.restartH264();
    else if (!this.jpeg) this.restartJpeg();
    else if (this.frame) this.jpeg.push(this.frame);
  }

  private send(v: Viewer, kind: number, data: Buffer) {
    if (v.ws.readyState !== v.ws.OPEN) return;
    if (v.ws.bufferedAmount > 4 << 20) {
      v.synced = false;
      return;
    }
    const out = Buffer.allocUnsafe(1 + data.length);
    out[0] = kind;
    data.copy(out, 1);
    v.ws.send(out);
    this.stats.bytes += out.length;
  }

  private restartH264() {
    this.h264?.removeAllListeners();
    this.h264?.stop();
    const enc = new H264Encoder(this.width, this.height, { encoder: this.opts.encoder, quality: this.opts.quality, fps: this.opts.maxFps });
    this.h264 = enc;
    for (const v of this.viewers) if (v.format === "h264") v.synced = false;
    enc.on("config", (c: H264Config) => {
      const head = Buffer.from(JSON.stringify({ codec: c.codec, width: this.width, height: this.height }));
      const msg = Buffer.concat([head, Buffer.from([0]), c.description]);
      for (const v of this.viewers) if (v.format === "h264") this.send(v, MSG_CONFIG, msg);
    });
    enc.on("chunk", (c: H264Chunk) => {
      this.stats.frames++;
      for (const v of this.viewers) {
        if (v.format !== "h264") continue;
        if (!v.synced && !c.key) continue;
        if (c.key) v.synced = true;
        this.send(v, c.key ? MSG_KEY : MSG_DELTA, c.data);
      }
      if ([...this.viewers].some((v) => v.format === "h264" && !v.synced && v.ws.bufferedAmount < 256 << 10))
        this.requestKeyframe();
    });
    enc.on("exit", () => {
      if (this.h264 === enc) this.h264 = null;
    });
    if (this.frame) enc.push(this.frame);
  }

  private keyTimer: NodeJS.Timeout | null = null;

  /* A viewer fell behind and dropped frames: start it again on a fresh keyframe. */
  private requestKeyframe() {
    if (this.keyTimer) return;
    this.keyTimer = setTimeout(() => {
      this.keyTimer = null;
      if (this.h264) this.restartH264();
    }, 300);
  }

  private restartJpeg() {
    this.jpeg?.removeAllListeners();
    this.jpeg?.stop();
    const enc = new JpegEncoder(this.width, this.height, this.opts.quality);
    this.jpeg = enc;
    enc.on("jpeg", (j: Buffer) => {
      for (const v of this.viewers) if (v.format === "jpeg") this.send(v, MSG_JPEG, j);
    });
    enc.on("exit", () => {
      if (this.jpeg === enc) this.jpeg = null;
    });
    if (this.frame) enc.push(this.frame);
  }

  private reapEncoders() {
    const formats = new Set([...this.viewers].map((v) => v.format));
    if (!formats.has("h264") && this.h264) {
      this.h264.removeAllListeners();
      this.h264.stop();
      this.h264 = null;
    }
    if (!formats.has("jpeg") && this.jpeg) {
      this.jpeg.removeAllListeners();
      this.jpeg.stop();
      this.jpeg = null;
    }
  }

  get wantsVideo() {
    return this.viewers.size > 0;
  }

  addAudio(ws: WebSocket) {
    this.audioClients.add(ws);
    ws.on("close", () => this.audioClients.delete(ws));
  }

  onAudio(pcm: Buffer) {
    for (const ws of this.audioClients) {
      if (ws.readyState === ws.OPEN && ws.bufferedAmount < 1 << 20) ws.send(pcm);
    }
  }

  /** The emulator went away: keep viewers, drop the picture. */
  reset() {
    this.frame = null;
  }

  shutdown() {
    this.h264?.stop();
    this.jpeg?.stop();
  }
}
