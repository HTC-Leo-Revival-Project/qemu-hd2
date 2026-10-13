import { spawn, type ChildProcessWithoutNullStreams } from "node:child_process";
import { EventEmitter } from "node:events";
import type { VideoEncoder } from "../lib/emulator-types.ts";

/*
 * Frames in (BGRX), H.264 access units out.  ffmpeg muxes to FLV because
 * every FLV tag carries its own length, so each frame can be forwarded the
 * moment ffmpeg writes it (Annex B would hold the last frame back until the
 * next start code arrives).  The FLV sequence header is the avcC record that
 * WebCodecs takes as the decoder description.
 *
 * Events: "config" ({ codec, description }), "chunk" ({ key, data }), "exit".
 */

export type H264Config = { codec: string; description: Buffer };
export type H264Chunk = { key: boolean; data: Buffer };

export class H264Encoder extends EventEmitter {
  private proc: ChildProcessWithoutNullStreams;
  private buf: Buffer = Buffer.alloc(0);
  private headerDone = false;
  private busy = false;
  private pending: Buffer | null = null;
  config: H264Config | null = null;
  readonly width: number;
  readonly height: number;

  constructor(width: number, height: number, opts: { encoder: VideoEncoder; quality: number; fps: number }) {
    super();
    this.width = width;
    this.height = height;
    const codec =
      opts.encoder === "nvenc"
        ? ["-c:v", "h264_nvenc", "-preset", "p1", "-tune", "ull", "-zerolatency", "1", "-delay", "0",
           "-rc", "constqp", "-qp", String(opts.quality), "-bf", "0", "-g", "600", "-profile:v", "high"]
        : ["-c:v", "libx264", "-preset", "superfast", "-tune", "zerolatency", "-threads", "1",
           "-crf", String(opts.quality), "-g", "600", "-bf", "0", "-profile:v", "high"];
    this.proc = spawn(
      "ffmpeg",
      ["-hide_banner", "-loglevel", "error", "-nostdin",
       "-f", "rawvideo", "-pix_fmt", "bgr0", "-s", `${width}x${height}`, "-framerate", String(opts.fps), "-i", "pipe:0",
       "-an", ...codec, "-pix_fmt", "yuv420p",
       "-f", "flv", "-flvflags", "no_duration_filesize", "-flush_packets", "1", "pipe:1"],
      { stdio: ["pipe", "pipe", "pipe"] },
    );
    this.proc.stdout.on("data", (d: Buffer) => this.onData(d));
    this.proc.stderr.on("data", (d: Buffer) => process.stderr.write(`[ffmpeg] ${d}`));
    this.proc.stdin.on("error", () => {});
    this.proc.on("exit", () => this.emit("exit"));
  }

  /** Queue a frame; while ffmpeg is still taking the previous one, only the newest is kept. */
  push(frame: Buffer) {
    if (this.busy) {
      this.pending = Buffer.from(frame);
      return;
    }
    this.busy = true;
    const ok = this.proc.stdin.write(Buffer.from(frame), () => {
      this.busy = false;
      const next = this.pending;
      this.pending = null;
      if (next) this.push(next);
    });
    void ok;
  }

  private onData(d: Buffer) {
    this.buf = this.buf.length ? Buffer.concat([this.buf, d]) : d;
    if (!this.headerDone) {
      if (this.buf.length < 13) return;
      this.buf = this.buf.subarray(13);
      this.headerDone = true;
    }
    for (;;) {
      if (this.buf.length < 11) return;
      const type = this.buf[0];
      const size = this.buf.readUIntBE(1, 3);
      const total = 11 + size + 4;
      if (this.buf.length < total) return;
      const body = this.buf.subarray(11, 11 + size);
      this.buf = this.buf.subarray(total);
      if (type === 9 && body.length > 5) this.onVideoTag(body);
    }
  }

  private onVideoTag(body: Buffer) {
    const key = body[0] >> 4 === 1;
    const pktType = body[1];
    const payload = Buffer.from(body.subarray(5));
    if (pktType === 0) {
      const hex = (b: number) => b.toString(16).padStart(2, "0");
      this.config = { codec: `avc1.${hex(payload[1])}${hex(payload[2])}${hex(payload[3])}`, description: payload };
      this.emit("config", this.config);
    } else if (pktType === 1) {
      this.emit("chunk", { key, data: payload } satisfies H264Chunk);
    }
  }

  stop() {
    this.proc.stdin.end();
    this.proc.kill("SIGKILL");
  }
}

/* Fallback for browsers without WebCodecs (e.g. plain http from another machine): JPEG frames. */
export class JpegEncoder extends EventEmitter {
  private proc: ChildProcessWithoutNullStreams;
  private buf: Buffer = Buffer.alloc(0);
  private busy = false;
  private pending: Buffer | null = null;

  constructor(width: number, height: number, quality: number) {
    super();
    const q = Math.max(2, Math.min(15, Math.round(quality / 4)));
    this.proc = spawn(
      "ffmpeg",
      ["-hide_banner", "-loglevel", "error", "-nostdin",
       "-f", "rawvideo", "-pix_fmt", "bgr0", "-s", `${width}x${height}`, "-i", "pipe:0",
       "-an", "-c:v", "mjpeg", "-q:v", String(q), "-pix_fmt", "yuvj420p",
       "-f", "image2pipe", "-flush_packets", "1", "pipe:1"],
      { stdio: ["pipe", "pipe", "pipe"] },
    );
    this.proc.stdout.on("data", (d: Buffer) => this.onData(d));
    this.proc.stderr.on("data", (d: Buffer) => process.stderr.write(`[ffmpeg] ${d}`));
    this.proc.stdin.on("error", () => {});
    this.proc.on("exit", () => this.emit("exit"));
  }

  push(frame: Buffer) {
    if (this.busy) {
      this.pending = Buffer.from(frame);
      return;
    }
    this.busy = true;
    this.proc.stdin.write(Buffer.from(frame), () => {
      this.busy = false;
      const next = this.pending;
      this.pending = null;
      if (next) this.push(next);
    });
  }

  private onData(d: Buffer) {
    this.buf = this.buf.length ? Buffer.concat([this.buf, d]) : d;
    for (;;) {
      const end = this.buf.indexOf(Buffer.from([0xff, 0xd9]));
      if (end < 0) return;
      this.emit("jpeg", Buffer.from(this.buf.subarray(0, end + 2)));
      this.buf = this.buf.subarray(end + 2);
    }
  }

  stop() {
    this.proc.stdin.end();
    this.proc.kill("SIGKILL");
  }
}
