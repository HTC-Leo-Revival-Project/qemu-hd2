import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import { execFile } from "node:child_process";
import next from "next";
import { WebSocketServer, type WebSocket } from "ws";
import { IMAGES, WEB_DIR } from "./paths.ts";
import { firmware, listImages, loadConfig, sanitize, saveConfig } from "./config.ts";
import { Emulator } from "./emulator.ts";
import { StreamHub } from "./stream.ts";
import type { Cable, InputMessage, LogName, ServerState } from "../lib/emulator-types.ts";

const dev = process.env.NODE_ENV !== "production";
const port = Number(process.env.PORT || 3000);
const host = process.env.HOST || "127.0.0.1";

let config = loadConfig();
const hub = new StreamHub(config);
const emu = new Emulator(hub);

const state = (): ServerState => ({ config, images: listImages(), firmware: firmware(), status: emu.status() });

const LOGS: Record<LogName, string> = { uart1: "uart1.log", ramcon: "ramcon.log", qemu: "qemu.out", q6: "q6.out" };

const readBody = (req: http.IncomingMessage) =>
  new Promise<unknown>((resolve, reject) => {
    let s = "";
    req.on("data", (d) => {
      s += d;
      if (s.length > 1 << 16) req.destroy();
    });
    req.on("end", () => {
      try {
        resolve(s ? JSON.parse(s) : {});
      } catch (e) {
        reject(e);
      }
    });
    req.on("error", reject);
  });

const json = (res: http.ServerResponse, code: number, body: unknown) => {
  res.writeHead(code, { "content-type": "application/json", "cache-control": "no-store" });
  res.end(JSON.stringify(body));
};

const crcTable = new Int32Array(256).map((_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c;
});
const crc32 = (b: Buffer) => {
  let c = -1;
  for (const x of b) c = crcTable[(c ^ x) & 0xff] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
};
const pngChunk = (type: string, data: Buffer) => {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, "latin1"), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
};
const toPng = (fb: Buffer, w: number, h: number) => {
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) {
    const o = y * (w * 3 + 1);
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      raw[o + 1 + x * 3] = fb[i + 2];
      raw[o + 2 + x * 3] = fb[i + 1];
      raw[o + 3 + x * 3] = fb[i];
    }
  }
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(w, 0);
  ihdr.writeUInt32BE(h, 4);
  ihdr[8] = 8;
  ihdr[9] = 2;
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    pngChunk("IHDR", ihdr),
    pngChunk("IDAT", zlib.deflateSync(raw)),
    pngChunk("IEND", Buffer.alloc(0)),
  ]);
};

/* Typing goes through adb: the HD2 has no keyboard, only its seven keys. */
const adb = (args: string[]) =>
  new Promise<void>((resolve, reject) => {
    const serial = `127.0.0.1:${emu.status().running?.adbPort ?? config.adbPort}`;
    execFile("adb", ["connect", serial], { timeout: 5000 }, () => {
      execFile("adb", ["-s", serial, ...args], { timeout: 10000 }, (err, _o, stderr) =>
        err ? reject(new Error(stderr.trim() || err.message)) : resolve(),
      );
    });
  });

const adbText = (text: string) =>
  adb(["shell", "input", "text", text.replace(/ /g, "%s").replace(/(["'`\\$&|;<>()*?!#~])/g, "\\$1")]);

const api = async (req: http.IncomingMessage, res: http.ServerResponse, url: URL) => {
  const route = `${req.method} ${url.pathname}`;
  try {
    switch (route) {
      case "GET /api/state":
        return json(res, 200, state());
      case "PUT /api/config": {
        config = sanitize((await readBody(req)) as object, config);
        saveConfig(config);
        hub.setOptions(config);
        broadcastStatus();
        return json(res, 200, state());
      }
      case "POST /api/power": {
        const { action } = (await readBody(req)) as { action?: string };
        if (action === "start") void emu.start(config).catch(() => {});
        else if (action === "stop") void emu.stop().catch(() => {});
        else if (action === "restart") void emu.restart(config).catch(() => {});
        else if (action === "reset") await emu.reset();
        else return json(res, 400, { error: "unknown action" });
        return json(res, 202, state());
      }
      case "POST /api/sensors": {
        const b = (await readBody(req)) as { cable?: Cable; battery?: number; gps?: { lat: number; lon: number; alt: number } };
        if (b.cable) await emu.setCable(b.cable);
        if (typeof b.battery === "number") await emu.setBattery(b.battery);
        if (b.gps) await emu.setGps(Number(b.gps.lat), Number(b.gps.lon), Number(b.gps.alt));
        return json(res, 200, state());
      }
      case "POST /api/text": {
        const { text } = (await readBody(req)) as { text?: string };
        if (!text) return json(res, 400, { error: "no text" });
        await adbText(text.slice(0, 500));
        return json(res, 200, { ok: true });
      }
      case "POST /api/keyevent": {
        const { code } = (await readBody(req)) as { code?: number };
        if (!Number.isInteger(code) || (code as number) < 0 || (code as number) > 300) return json(res, 400, { error: "bad key code" });
        await adb(["shell", "input", "keyevent", String(code)]);
        return json(res, 200, { ok: true });
      }
      case "GET /api/screenshot": {
        const s = emu.screen;
        if (!s) return json(res, 404, { error: "no picture yet" });
        res.writeHead(200, {
          "content-type": "image/png",
          "content-disposition": `attachment; filename="hd2-${new Date().toISOString().replace(/[:.]/g, "-")}.png"`,
        });
        return res.end(toPng(s.fb, s.w, s.h));
      }
      case "GET /api/log": {
        const name = url.searchParams.get("name") as LogName;
        const file = LOGS[name];
        if (!file) return json(res, 400, { error: "unknown log" });
        let text = "";
        try {
          const p = path.join(IMAGES, file);
          const size = fs.statSync(p).size;
          const len = Math.min(size, 64 << 10);
          const fd = fs.openSync(p, "r");
          const buf = Buffer.alloc(len);
          fs.readSync(fd, buf, 0, len, size - len);
          fs.closeSync(fd);
          text = buf.toString("utf8");
        } catch {
          text = "";
        }
        res.writeHead(200, { "content-type": "text/plain; charset=utf-8", "cache-control": "no-store" });
        return res.end(text);
      }
    }
    json(res, 404, { error: "not found" });
  } catch (e) {
    json(res, 500, { error: (e as Error).message });
  }
};

const app = next({ dev, dir: WEB_DIR, hostname: host, port });
const handle = app.getRequestHandler();
await app.prepare();
const nextUpgrade = app.getUpgradeHandler();

const server = http.createServer((req, res) => {
  const url = new URL(req.url ?? "/", "http://localhost");
  if (url.pathname.startsWith("/api/")) return void api(req, res, url);
  void handle(req, res);
});

const wss = new WebSocketServer({ noServer: true, perMessageDeflate: false });
const controls = new Set<WebSocket>();

const broadcastStatus = () => {
  const msg = JSON.stringify({ t: "status", status: emu.status(), config });
  for (const ws of controls) if (ws.readyState === ws.OPEN) ws.send(msg);
};
emu.on("status", broadcastStatus);
setInterval(() => {
  if (controls.size) broadcastStatus();
}, 1000);

server.on("upgrade", (req, sock, head) => {
  const url = new URL(req.url ?? "/", "http://localhost");
  if (!url.pathname.startsWith("/ws/")) return void nextUpgrade(req, sock, head);
  wss.handleUpgrade(req, sock, head, (ws) => {
    switch (url.pathname) {
      case "/ws/video":
        hub.addViewer(ws, url.searchParams.get("format") === "jpeg" ? "jpeg" : "h264");
        break;
      case "/ws/audio":
        hub.addAudio(ws);
        break;
      case "/ws/control":
        controls.add(ws);
        ws.on("close", () => controls.delete(ws));
        ws.on("message", (d, isBinary) => {
          if (isBinary) return;
          try {
            emu.input(JSON.parse(d.toString()) as InputMessage);
          } catch {
            /* ignore malformed input */
          }
        });
        ws.send(JSON.stringify({ t: "status", status: emu.status(), config }));
        break;
      default:
        ws.close();
    }
  });
});

await emu.adopt().catch((e) => console.error("adopt:", e.message));

server.listen(port, host, () => {
  console.log(`> HD2 web UI on http://${host}:${port} (${dev ? "development" : "production"})`);
});

for (const sig of ["SIGINT", "SIGTERM"] as const)
  process.on(sig, () => {
    hub.shutdown();
    process.exit(0);
  });
