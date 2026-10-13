"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import type { EmulatorConfig, EmulatorStatus, InputMessage, ServerState } from "./emulator-types";

const wsUrl = (path: string) => `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}${path}`;

export const api = async <T = ServerState>(path: string, init?: { method?: string; body?: unknown }): Promise<T> => {
  const r = await fetch(path, {
    method: init?.method ?? "GET",
    headers: init?.body ? { "content-type": "application/json" } : undefined,
    body: init?.body ? JSON.stringify(init.body) : undefined,
    cache: "no-store",
  });
  const data = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error((data as { error?: string }).error ?? r.statusText);
  return data as T;
};

/** Keeps a WebSocket open, reconnecting with backoff. */
const useSocket = (path: string | null, onMessage: (ev: MessageEvent, ws: WebSocket) => void, binary = false) => {
  const ref = useRef<WebSocket | null>(null);
  const handler = useRef(onMessage);
  const [open, setOpen] = useState(false);
  useEffect(() => {
    handler.current = onMessage;
  });
  useEffect(() => {
    if (!path) return;
    let stop = false;
    let retry = 500;
    let timer: ReturnType<typeof setTimeout>;
    const connect = () => {
      const ws = new WebSocket(wsUrl(path));
      if (binary) ws.binaryType = "arraybuffer";
      ref.current = ws;
      ws.onopen = () => {
        retry = 500;
        setOpen(true);
      };
      ws.onmessage = (ev) => handler.current(ev, ws);
      ws.onclose = () => {
        setOpen(false);
        if (ref.current === ws) ref.current = null;
        if (!stop) timer = setTimeout(connect, (retry = Math.min(retry * 2, 4000)));
      };
    };
    connect();
    return () => {
      stop = true;
      clearTimeout(timer);
      ref.current?.close();
    };
  }, [path, binary]);
  return { ref, open };
};

export const useControl = () => {
  const [status, setStatus] = useState<EmulatorStatus | null>(null);
  const [config, setConfig] = useState<EmulatorConfig | null>(null);
  const { ref, open } = useSocket("/ws/control", (ev) => {
    const m = JSON.parse(ev.data as string) as { t: string; status: EmulatorStatus; config: EmulatorConfig };
    if (m.t === "status") {
      setStatus(m.status);
      setConfig(m.config);
    }
  });
  const send = useCallback(
    (m: InputMessage) => {
      const ws = ref.current;
      if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(m));
    },
    [ref],
  );
  return { status, config, setConfig, send, connected: open };
};

export type VideoStats = { fps: number; decodeMs: number; mode: "h264" | "jpeg" | "none"; codec: string; lastFrameAt: number };

const paint = (c: HTMLCanvasElement | null, img: CanvasImageSource, w: number, h: number) => {
  if (!c) return false;
  if (c.width !== w || c.height !== h) {
    c.width = w;
    c.height = h;
  }
  c.getContext("2d")?.drawImage(img, 0, 0, w, h);
  return true;
};

/*
 * The screen: H.264 decoded by WebCodecs straight onto a canvas, or JPEG
 * pictures where WebCodecs is missing (it needs a secure context, so a plain
 * http page opened from another machine falls back).
 */
export const useVideo = (canvas: React.RefObject<HTMLCanvasElement | null>) => {
  const [mode] = useState<"h264" | "jpeg">(() =>
    typeof window !== "undefined" && "VideoDecoder" in window ? "h264" : "jpeg",
  );
  const [stats, setStats] = useState<VideoStats>({ fps: 0, decodeMs: 0, mode: "none", codec: "", lastFrameAt: 0 });
  const dec = useRef<VideoDecoder | null>(null);
  const counters = useRef({ frames: 0, decodeSum: 0, t: 0, codec: "", last: 0 });
  const pendingT = useRef(new Map<number, number>());
  const draw = useCallback(
    (img: CanvasImageSource, w: number, h: number) => {
      if (!paint(canvas.current, img, w, h)) return;
      counters.current.frames++;
      counters.current.last = Date.now();
    },
    [canvas],
  );

  useSocket(
    `/ws/video?format=${mode}`,
    (ev, ws) => {
      const buf = new Uint8Array(ev.data as ArrayBuffer);
      const kind = buf[0];
      if (kind === 1) {
        const z = buf.indexOf(0, 1);
        const cfg = JSON.parse(new TextDecoder().decode(buf.subarray(1, z))) as { codec: string; width: number; height: number };
        dec.current?.close();
        const d = new VideoDecoder({
          output: (frame) => {
            const t0 = pendingT.current.get(frame.timestamp);
            pendingT.current.delete(frame.timestamp);
            if (t0 !== undefined) counters.current.decodeSum += performance.now() - t0;
            draw(frame, frame.displayWidth, frame.displayHeight);
            frame.close();
          },
          error: () => {
            /* reconnecting makes the server start over on a keyframe */
            dec.current = null;
            ws.close();
          },
        });
        d.configure({ codec: cfg.codec, description: buf.slice(z + 1), optimizeForLatency: true, codedWidth: cfg.width, codedHeight: cfg.height });
        dec.current = d;
        counters.current.codec = cfg.codec;
      } else if (kind === 2 || kind === 3) {
        const d = dec.current;
        if (!d || d.state !== "configured") return;
        const ts = Math.round(performance.now() * 1000);
        pendingT.current.set(ts, performance.now());
        if (pendingT.current.size > 64) pendingT.current.clear();
        d.decode(new EncodedVideoChunk({ type: kind === 2 ? "key" : "delta", timestamp: ts, data: buf.subarray(1) }));
      } else if (kind === 4) {
        createImageBitmap(new Blob([buf.subarray(1)], { type: "image/jpeg" })).then((bmp) => {
          draw(bmp, bmp.width, bmp.height);
          bmp.close();
        });
      }
    },
    true,
  );

  useEffect(() => {
    counters.current.t = performance.now();
    const iv = setInterval(() => {
      const c = counters.current;
      const now = performance.now();
      const dt = (now - c.t) / 1000;
      setStats({ fps: Math.round(c.frames / dt), decodeMs: c.frames ? +(c.decodeSum / c.frames).toFixed(1) : 0, mode, codec: c.codec, lastFrameAt: c.last });
      c.frames = 0;
      c.decodeSum = 0;
      c.t = now;
    }, 1000);
    return () => {
      clearInterval(iv);
      dec.current?.close();
    };
  }, [mode]);

  return stats;
};

/* The emulator's audio: 48 kHz stereo S16 PCM through an AudioWorklet ring buffer. */
export const useAudio = () => {
  const [enabled, setEnabled] = useState(false);
  const node = useRef<AudioWorkletNode | null>(null);
  const ctx = useRef<AudioContext | null>(null);

  const enable = useCallback(async () => {
    if (!ctx.current) {
      const c = new AudioContext({ sampleRate: 48000, latencyHint: "interactive" });
      await c.audioWorklet.addModule("/pcm-player.js");
      const n = new AudioWorkletNode(c, "pcm-player", { outputChannelCount: [2] });
      n.connect(c.destination);
      ctx.current = c;
      node.current = n;
    }
    await ctx.current.resume();
    setEnabled(true);
  }, []);

  const disable = useCallback(async () => {
    await ctx.current?.suspend();
    setEnabled(false);
  }, []);

  useSocket(
    enabled ? "/ws/audio" : null,
    (ev) => {
      const pcm = ev.data as ArrayBuffer;
      node.current?.port.postMessage(pcm, [pcm]);
    },
    true,
  );

  useEffect(() => () => void ctx.current?.close(), []);

  return { enabled, enable, disable };
};
