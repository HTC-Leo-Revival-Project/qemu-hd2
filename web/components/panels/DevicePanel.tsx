"use client";

import { useState } from "react";
import { Camera, Power, RefreshCw, RotateCcw, Send, Square } from "lucide-react";
import { Category, HoloButton, PrefRow } from "@/components/holo";
import type { EmulatorStatus, HardKey } from "@/lib/emulator-types";

const KEYS: { key: HardKey; label: string; hint: string }[] = [
  { key: "call", label: "Call", hint: "F2" },
  { key: "home", label: "Home", hint: "Home" },
  { key: "menu", label: "Windows", hint: "F1" },
  { key: "back", label: "Back", hint: "Esc" },
  { key: "end", label: "End", hint: "F3" },
  { key: "power", label: "Power", hint: "F12" },
  { key: "volup", label: "Volume +", hint: "PgUp" },
  { key: "voldown", label: "Volume −", hint: "PgDn" },
];

const STATE_LABEL: Record<EmulatorStatus["state"], string> = {
  stopped: "Powered off",
  starting: "Starting",
  running: "Running",
  stopping: "Shutting down",
  crashed: "Stopped with an error",
};

export const stateColor = (s: EmulatorStatus["state"] | undefined) =>
  s === "running" ? "var(--holo-green)" : s === "crashed" ? "var(--holo-red)" : s === "starting" || s === "stopping" ? "var(--holo-orange)" : "#6b6b6b";

const uptime = (since: number) => {
  const s = Math.max(0, Math.floor((Date.now() - since) / 1000));
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
  return h ? `${h} h ${m} min` : m ? `${m} min ${s % 60} s` : `${s} s`;
};

const modeLine = (st: EmulatorStatus) => {
  const r = st.running;
  if (!r) return st.external ? "Started outside the web UI" : "";
  const bb = r.baseband === "lle" ? "Real AMSS baseband" : "Modelled baseband";
  const dsp = r.dsp === "lle" ? ", real DSP1 on Hexagon" : "";
  return `${bb}${dsp}, ${r.nand}${r.sdcard ? ` + ${r.sdcard}` : ""}`;
};

type Props = {
  status: EmulatorStatus | null;
  pressed: Set<HardKey>;
  onKey: (k: HardKey, down: boolean) => void;
  onPower: (a: "start" | "stop" | "restart" | "reset") => void;
  onType: (text: string) => Promise<void>;
  video: { fps: number; decodeMs: number; mode: string; codec: string };
};

export const DevicePanel = ({ status, pressed, onKey, onPower, onType, video }: Props) => {
  const [text, setText] = useState("");
  const [sending, setSending] = useState(false);
  const st = status?.state ?? "stopped";
  const on = st === "running";
  const busy = st === "starting" || st === "stopping";

  return (
    <div className="pb-8">
      <Category>Status</Category>
      <div className="flex items-start gap-4 px-4 py-3">
        <span className="mt-1.5 size-3 shrink-0 rounded-full" style={{ background: stateColor(st), boxShadow: `0 0 10px ${stateColor(st)}` }} />
        <div className="min-w-0 flex-1">
          <div className="text-[19px] font-light">{STATE_LABEL[st]}</div>
          {status && on && <div className="text-[13.5px] text-holo-text-3">Up {uptime(status.since)}</div>}
          {status && <div className="mt-1 text-[13.5px] text-holo-text-2">{modeLine(status)}</div>}
          {status?.message && (
            <pre className="mt-2 max-h-40 overflow-auto whitespace-pre-wrap border-l-2 border-holo-orange/70 pl-3 font-mono text-[12px] text-holo-text-2">
              {status.message}
            </pre>
          )}
        </div>
      </div>
      {on && (
        <div className="grid grid-cols-3 gap-px bg-holo-divider px-0 py-px">
          {[
            ["Screen", `${video.fps} fps`],
            ["Stream", `${status?.stream.kbps ?? 0} kb/s`],
            ["Decode", video.mode === "h264" ? `${video.decodeMs} ms` : "JPEG"],
          ].map(([k, v]) => (
            <div key={k} className="bg-black/50 px-4 py-2.5">
              <div className="text-[11px] uppercase tracking-[0.08em] text-holo-text-3">{k}</div>
              <div className="font-mono text-[15px] text-holo-blue">{v}</div>
            </div>
          ))}
        </div>
      )}

      <div className="grid grid-cols-2 gap-2 px-4 pt-4">
        {on || busy ? (
          <HoloButton onClick={() => onPower("stop")} disabled={busy}>
            <Square size={16} strokeWidth={1.75} /> Shut down
          </HoloButton>
        ) : (
          <HoloButton primary onClick={() => onPower("start")}>
            <Power size={18} strokeWidth={1.75} /> Power on
          </HoloButton>
        )}
        <HoloButton onClick={() => onPower("restart")} disabled={busy}>
          <RefreshCw size={16} strokeWidth={1.75} /> Restart QEMU
        </HoloButton>
        <HoloButton onClick={() => onPower("reset")} disabled={!on} title="Reset the board without restarting QEMU">
          <RotateCcw size={16} strokeWidth={1.75} /> Reset board
        </HoloButton>
        <a
          href="/api/screenshot"
          className={`inline-flex min-h-12 items-center justify-center gap-2 border border-white/10 bg-white/[0.07] px-4 text-[15px] hover:bg-white/[0.11] active:bg-[var(--holo-pressed)] ${on ? "" : "pointer-events-none opacity-35"}`}
        >
          <Camera size={16} strokeWidth={1.75} /> Screenshot
        </a>
      </div>

      <Category>Hardware keys</Category>
      <div className="grid grid-cols-4 gap-2 px-4 py-2">
        {KEYS.map((k) => (
          <button
            key={k.key}
            type="button"
            disabled={!on}
            data-down={pressed.has(k.key)}
            onPointerDown={(e) => {
              e.currentTarget.setPointerCapture(e.pointerId);
              onKey(k.key, true);
            }}
            onPointerUp={() => onKey(k.key, false)}
            onPointerCancel={() => onKey(k.key, false)}
            className="flex min-h-[58px] touch-none select-none flex-col items-center justify-center border border-white/10 bg-white/[0.06] text-[14px] transition-colors hover:bg-white/[0.1] disabled:opacity-35 data-[down=true]:bg-[var(--holo-pressed)]"
          >
            <span>{k.label}</span>
            <span className="font-mono text-[10.5px] uppercase tracking-wider text-holo-text-3">{k.hint}</span>
          </button>
        ))}
      </div>
      <p className="px-4 pt-1 text-[12.5px] leading-5 text-holo-text-3">
        Hold a key to long-press it. With the screen focused, the keyboard shortcuts work too, and typed text goes to Android
        over adb.
      </p>

      <Category>Type into Android</Category>
      <form
        className="flex items-end gap-3 px-4 py-3"
        onSubmit={async (e) => {
          e.preventDefault();
          if (!text) return;
          setSending(true);
          await onType(text).finally(() => setSending(false));
          setText("");
        }}
      >
        <input
          className="holo-field min-w-0 flex-1 text-[16px]"
          placeholder="Text for the focused field"
          value={text}
          disabled={!on}
          onChange={(e) => setText(e.target.value)}
        />
        <HoloButton type="submit" disabled={!on || !text || sending} className="min-h-10 px-3">
          <Send size={16} strokeWidth={1.75} /> Send
        </HoloButton>
      </form>

      <Category>About</Category>
      <PrefRow
        title="HTC HD2 (htcleo) on QEMU"
        summary="Qualcomm QSD8250, Scorpion 1 GHz, 4.3-inch 480 × 800 panel. Screen streamed as H.264 to WebCodecs."
      />
      <PrefRow
        title="Device photo"
        summary={
          <>
            “HTC HD2 - Front” by Beamish4,{" "}
            <a className="text-holo-blue underline" href="https://commons.wikimedia.org/wiki/File:HTC_HD2_-_Front.jpg" target="_blank" rel="noreferrer">
              Wikimedia Commons
            </a>
            , CC BY-SA 4.0, straightened and cut out.
          </>
        }
      />
    </div>
  );
};
