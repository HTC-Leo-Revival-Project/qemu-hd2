import net from "node:net";
import { EventEmitter } from "node:events";

/*
 * A minimal RFB 3.8 client for QEMU's VNC server on a unix socket: raw
 * 32-bit BGRX framebuffer updates, desktop resizes, and QEMU's audio
 * extension (signed 16-bit stereo PCM at 48 kHz).
 *
 * Events: "frame" (fb: Buffer, w, h) after each complete update,
 * "audio" (pcm: Buffer), "close".
 */

const ENC_RAW = 0;
const ENC_DESKTOP_SIZE = -223;
const ENC_QEMU_AUDIO = -259;

export class VncClient extends EventEmitter {
  width = 0;
  height = 0;
  fb: Buffer = Buffer.alloc(0);
  private sock: net.Socket;
  private chunks: Buffer[] = [];
  private avail = 0;
  private want = 0;
  private waiter: ((b: Buffer) => void) | null = null;
  private closed = false;

  private constructor(sock: net.Socket) {
    super();
    this.sock = sock;
    sock.setNoDelay(true);
    sock.on("data", (d) => {
      this.chunks.push(d);
      this.avail += d.length;
      this.pump();
    });
    sock.on("close", () => this.close());
    sock.on("error", () => this.close());
  }

  static connect(path: string, opts: { audio: boolean; video: boolean }): Promise<VncClient> {
    return new Promise((resolve, reject) => {
      const sock = net.createConnection(path);
      const c = new VncClient(sock);
      sock.once("error", reject);
      sock.once("connect", () => {
        c.handshake(opts)
          .then(() => {
            sock.off("error", reject);
            resolve(c);
            c.loop().catch(() => c.close());
          })
          .catch((e) => {
            c.close();
            reject(e);
          });
      });
    });
  }

  private pump() {
    if (!this.waiter || this.avail < this.want) return;
    const n = this.want;
    let out: Buffer;
    if (this.chunks[0].length >= n) {
      out = this.chunks[0].subarray(0, n);
      if (this.chunks[0].length === n) this.chunks.shift();
      else this.chunks[0] = this.chunks[0].subarray(n);
    } else {
      out = Buffer.allocUnsafe(n);
      let off = 0;
      while (off < n) {
        const c = this.chunks[0];
        const take = Math.min(c.length, n - off);
        c.copy(out, off, 0, take);
        off += take;
        if (take === c.length) this.chunks.shift();
        else this.chunks[0] = c.subarray(take);
      }
    }
    this.avail -= n;
    const w = this.waiter;
    this.waiter = null;
    w(out);
  }

  private read(n: number): Promise<Buffer> {
    if (this.closed) return Promise.reject(new Error("VNC closed"));
    if (n === 0) return Promise.resolve(Buffer.alloc(0));
    return new Promise((resolve) => {
      this.want = n;
      this.waiter = resolve;
      this.pump();
    });
  }

  private async handshake(opts: { audio: boolean; video: boolean }) {
    const ver = (await this.read(12)).toString("latin1");
    if (!ver.startsWith("RFB ")) throw new Error("not a VNC server");
    this.sock.write("RFB 003.008\n");
    const ntypes = (await this.read(1))[0];
    if (ntypes === 0) {
      const len = (await this.read(4)).readUInt32BE(0);
      throw new Error((await this.read(len)).toString());
    }
    const types = await this.read(ntypes);
    if (!types.includes(1)) throw new Error("VNC server requires authentication");
    this.sock.write(Buffer.from([1]));
    if ((await this.read(4)).readUInt32BE(0) !== 0) throw new Error("VNC security handshake failed");
    this.sock.write(Buffer.from([1]));
    const init = await this.read(24);
    this.resize(init.readUInt16BE(0), init.readUInt16BE(2));
    await this.read(init.readUInt32BE(20));

    const pf = Buffer.alloc(20);
    pf[0] = 0;
    pf[4] = 32; // bpp
    pf[5] = 24; // depth
    pf[6] = 0; // little endian
    pf[7] = 1; // true colour
    pf.writeUInt16BE(255, 8);
    pf.writeUInt16BE(255, 10);
    pf.writeUInt16BE(255, 12);
    pf[14] = 16;
    pf[15] = 8;
    pf[16] = 0;
    this.sock.write(pf);

    const encs = [ENC_RAW, ENC_DESKTOP_SIZE];
    if (opts.audio) encs.push(ENC_QEMU_AUDIO);
    const se = Buffer.alloc(4 + 4 * encs.length);
    se[0] = 2;
    se.writeUInt16BE(encs.length, 2);
    encs.forEach((e, i) => se.writeInt32BE(e, 4 + 4 * i));
    this.sock.write(se);

    if (opts.audio) {
      const fmt = Buffer.alloc(10);
      fmt[0] = 255;
      fmt[1] = 1;
      fmt.writeUInt16BE(2, 2);
      fmt[4] = 3; // S16
      fmt[5] = 2; // stereo
      fmt.writeUInt32BE(48000, 6);
      this.sock.write(fmt);
      this.sock.write(Buffer.from([255, 1, 0, 0]));
    }
    if (opts.video) this.requestUpdate(false);
  }

  private resize(w: number, h: number) {
    this.width = w;
    this.height = h;
    this.fb = Buffer.alloc(w * h * 4);
  }

  requestUpdate(incremental: boolean) {
    if (this.closed) return;
    const b = Buffer.alloc(10);
    b[0] = 3;
    b[1] = incremental ? 1 : 0;
    b.writeUInt16BE(this.width, 6);
    b.writeUInt16BE(this.height, 8);
    this.sock.write(b);
  }

  private async loop() {
    for (;;) {
      const type = (await this.read(1))[0];
      switch (type) {
        case 0: {
          const hdr = await this.read(3);
          const n = hdr.readUInt16BE(1);
          let pixels = false;
          for (let i = 0; i < n; i++) {
            const r = await this.read(12);
            const x = r.readUInt16BE(0), y = r.readUInt16BE(2);
            const w = r.readUInt16BE(4), h = r.readUInt16BE(6);
            const enc = r.readInt32BE(8);
            if (enc === ENC_DESKTOP_SIZE) {
              this.resize(w, h);
            } else if (enc === ENC_QEMU_AUDIO) {
              /* the server's acknowledgement of the audio extension */
            } else if (enc === ENC_RAW) {
              pixels = true;
              const px = await this.read(w * h * 4);
              const stride = this.width * 4;
              if (x === 0 && w === this.width) px.copy(this.fb, y * stride);
              else
                for (let row = 0; row < h; row++)
                  px.copy(this.fb, (y + row) * stride + x * 4, row * w * 4, (row + 1) * w * 4);
            } else {
              throw new Error(`unexpected VNC encoding ${enc}`);
            }
          }
          if (pixels) this.emit("frame", this.fb, this.width, this.height);
          this.requestUpdate(true);
          break;
        }
        case 1: {
          const h = await this.read(5);
          await this.read(h.readUInt16BE(3) * 6);
          break;
        }
        case 2:
          break;
        case 3: {
          const h = await this.read(7);
          await this.read(h.readUInt32BE(3));
          break;
        }
        case 255: {
          const h = await this.read(3);
          if (h[0] !== 1) throw new Error(`unknown QEMU server message ${h[0]}`);
          const op = h.readUInt16BE(1);
          if (op === 2) {
            const len = (await this.read(4)).readUInt32BE(0);
            this.emit("audio", Buffer.from(await this.read(len)));
          }
          break;
        }
        default:
          throw new Error(`unknown VNC message ${type}`);
      }
    }
  }

  close() {
    if (this.closed) return;
    this.closed = true;
    this.sock.destroy();
    if (this.waiter) this.waiter = null;
    this.emit("close");
  }
}
