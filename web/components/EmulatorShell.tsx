"use client";

import { useCallback, useEffect, useRef, useState } from "react";
import { Camera, Maximize2, Minimize2, Power, Volume2, VolumeX } from "lucide-react";
import { DeviceFrame } from "@/components/device/DeviceFrame";
import { Dialog, Spinner, Toast } from "@/components/holo";
import { ConfigPanel } from "@/components/panels/ConfigPanel";
import { DevicePanel, stateColor } from "@/components/panels/DevicePanel";
import { LogsPanel } from "@/components/panels/LogsPanel";
import { SensorsPanel } from "@/components/panels/SensorsPanel";
import { api, useAudio, useControl, useVideo } from "@/lib/client";
import { FRAME, SCREEN } from "@/lib/frame-geometry";
import type { EmulatorConfig, HardKey, ServerState } from "@/lib/emulator-types";

const TABS = ["Device", "Config", "Sensors", "Logs"] as const;
type Tab = (typeof TABS)[number];

const KEYMAP: Record<string, HardKey> = {
  Escape: "back",
  Home: "home",
  F1: "menu",
  ContextMenu: "menu",
  F2: "call",
  F3: "end",
  F12: "power",
  PageUp: "volup",
  PageDown: "voldown",
};

/* Android key codes for adb input keyevent */
const ADB_KEYS: Record<string, number> = {
  Enter: 66,
  Backspace: 67,
  Delete: 112,
  Tab: 61,
  ArrowUp: 19,
  ArrowDown: 20,
  ArrowLeft: 21,
  ArrowRight: 22,
};

const RESTART_FIELDS: (keyof EmulatorConfig)[] = [
  "baseband", "dsp", "usb", "nand", "sdcard", "boot", "gpu", "audio", "wifi", "mobileData", "adbPort", "amssLog", "dspShift", "extraArgs",
];

const AppIcon = () => (
  <svg viewBox="0 0 32 32" className="size-8" aria-hidden>
    <rect x="8" y="3" width="16" height="26" rx="2.5" fill="none" stroke="var(--holo-blue)" strokeWidth="2" />
    <rect x="11" y="7" width="10" height="15" fill="var(--holo-blue)" opacity="0.35" />
    <circle cx="16" cy="25.5" r="1.4" fill="var(--holo-blue)" />
  </svg>
);

const ActionButton = ({ label, active, onClick, children }: { label: string; active?: boolean; onClick: () => void; children: React.ReactNode }) => (
  <button
    type="button"
    title={label}
    aria-label={label}
    onClick={onClick}
    className={`holo-pressable flex h-14 w-14 items-center justify-center ${active ? "text-holo-blue" : "text-holo-text"}`}
  >
    {children}
  </button>
);

const EmulatorShell = () => {
  const { status, config, setConfig, send, connected } = useControl();
  const [server, setServer] = useState<ServerState | null>(null);
  const [tab, setTab] = useState<Tab>("Device");
  const [toast, setToast] = useState<string | null>(null);
  const [pressed, setPressed] = useState<Set<HardKey>>(new Set());
  const [frameW, setFrameW] = useState(0);
  const [zoom, setZoom] = useState<"fit" | "1:1">("fit");
  const [restartAsk, setRestartAsk] = useState(false);
  const canvas = useRef<HTMLCanvasElement>(null);
  const stage = useRef<HTMLDivElement>(null);
  const focusTarget = useRef<HTMLDivElement>(null);
  const toastTimer = useRef<ReturnType<typeof setTimeout>>(undefined);
  const typed = useRef("");
  const typeTimer = useRef<ReturnType<typeof setTimeout>>(undefined);

  const video = useVideo(canvas);
  const audio = useAudio();

  const say = useCallback((t: string) => {
    setToast(t);
    clearTimeout(toastTimer.current);
    toastTimer.current = setTimeout(() => setToast(null), 2600);
  }, []);

  useEffect(() => {
    api("/api/state").then(setServer).catch((e) => say(e.message));
  }, [say]);

  const state = status?.state ?? "stopped";
  const running = state === "running";
  const hasPicture = running && !!status && video.lastFrameAt >= status.since;

  useEffect(() => {
    const el = stage.current;
    if (!el) return;
    const fit = () => {
      const r = el.getBoundingClientRect();
      const pad = r.width < 640 ? 16 : 40;
      const byH = ((r.height - pad * 2) * FRAME.w) / FRAME.h;
      const fitted = Math.max(200, Math.min(r.width - pad * 2, byH));
      setFrameW(zoom === "1:1" ? (480 / SCREEN.w) * FRAME.w : fitted);
    };
    fit();
    const ro = new ResizeObserver(fit);
    ro.observe(el);
    return () => ro.disconnect();
  }, [zoom]);

  const onKey = useCallback(
    (k: HardKey, down: boolean) => {
      setPressed((p) => {
        const n = new Set(p);
        if (down) n.add(k);
        else n.delete(k);
        return n;
      });
      send({ t: "key", k, d: down });
    },
    [send],
  );

  const power = useCallback(
    async (action: "start" | "stop" | "restart" | "reset") => {
      try {
        await api("/api/power", { method: "POST", body: { action } });
        say(action === "start" ? "Powering on" : action === "stop" ? "Shutting down" : action === "restart" ? "Restarting QEMU" : "Board reset");
      } catch (e) {
        say((e as Error).message);
      }
    },
    [say],
  );

  const typeText = useCallback(
    async (text: string) => {
      try {
        await api("/api/text", { method: "POST", body: { text } });
      } catch (e) {
        say(`adb: ${(e as Error).message}`);
      }
    },
    [say],
  );

  const onStageKey = (e: React.KeyboardEvent, down: boolean) => {
    if (!running) return;
    const hk = KEYMAP[e.key];
    if (hk) {
      e.preventDefault();
      if (e.repeat) return;
      onKey(hk, down);
      return;
    }
    if (!down || e.ctrlKey || e.metaKey || e.altKey) return;
    if (ADB_KEYS[e.key] !== undefined) {
      e.preventDefault();
      void api("/api/keyevent", { method: "POST", body: { code: ADB_KEYS[e.key] } }).catch((err) => say(`adb: ${err.message}`));
      return;
    }
    if (e.key.length === 1) {
      e.preventDefault();
      typed.current += e.key;
      clearTimeout(typeTimer.current);
      typeTimer.current = setTimeout(() => {
        const t = typed.current;
        typed.current = "";
        void typeText(t);
      }, 150);
    }
  };

  const saveConfig = async (patch: Partial<EmulatorConfig>) => {
    if (!config) return;
    const next = { ...config, ...patch };
    setConfig(next);
    try {
      const s = await api("/api/config", { method: "PUT", body: next });
      setServer(s);
      const r = s.status.running;
      if (r && RESTART_FIELDS.some((f) => r[f] !== s.config[f])) setRestartAsk(true);
    } catch (e) {
      say((e as Error).message);
    }
  };

  const postSensors = async (body: object, done: string) => {
    try {
      await api("/api/sensors", { method: "POST", body });
      say(done);
    } catch (e) {
      say((e as Error).message);
    }
  };

  const overlay =
    state === "starting" || state === "stopping" || (running && !hasPicture) ? (
      <div className="flex flex-col items-center gap-5 bg-black/70 p-6 text-center">
        <Spinner size={48} />
        <span className="text-[15px] text-holo-text-2">{state === "stopping" ? "Shutting down" : "Booting"}</span>
      </div>
    ) : !running ? (
      <div className="flex flex-col items-center gap-5 px-6 text-center">
        <button
          type="button"
          onClick={() => power("start")}
          className="group flex size-24 items-center justify-center rounded-full border-2 border-holo-blue/50 bg-holo-blue/10 transition hover:border-holo-blue hover:bg-holo-blue/20 hover:shadow-[0_0_30px_rgba(51,181,229,0.45)] active:bg-holo-blue/40"
          aria-label="Power on"
        >
          <Power size={44} strokeWidth={1.5} className="text-holo-blue" />
        </button>
        <span className="text-[17px] font-light text-holo-text">{state === "crashed" ? "Stopped with an error" : "Powered off"}</span>
        {config && (
          <span className="max-w-[80%] text-[13px] text-holo-text-3">
            {config.baseband === "lle" ? "Real AMSS" : "Modelled baseband"}
            {config.dsp === "lle" ? " + real DSP" : ""} · {config.nand}
          </span>
        )}
      </div>
    ) : null;

  return (
    <div className="flex h-full flex-col">
      <header className="relative z-20 flex h-14 shrink-0 items-center gap-3 bg-holo-bar px-3 shadow-[0_2px_6px_rgba(0,0,0,0.7)]">
        <AppIcon />
        <div className="min-w-0 flex-1 leading-tight">
          <h1 className="truncate text-[19px] font-normal">HD2 Emulator</h1>
          <div className="flex items-center gap-1.5 text-[12.5px] text-holo-text-3">
            <span className="size-2 rounded-full" style={{ background: stateColor(state) }} />
            {connected ? (status ? state[0].toUpperCase() + state.slice(1) : "…") : "Reconnecting to the server"}
            {running && video.fps > 0 && <span className="font-mono">· {video.fps} fps</span>}
          </div>
        </div>
        <div className="flex items-center">
          <ActionButton
            label={audio.enabled ? "Mute browser audio" : "Play audio in this browser"}
            active={audio.enabled}
            onClick={() => void (audio.enabled ? audio.disable() : audio.enable())}
          >
            {audio.enabled ? <Volume2 size={22} strokeWidth={1.6} /> : <VolumeX size={22} strokeWidth={1.6} />}
          </ActionButton>
          <ActionButton label={zoom === "fit" ? "Show at 100% (480 × 800)" : "Fit to window"} onClick={() => setZoom(zoom === "fit" ? "1:1" : "fit")}>
            {zoom === "fit" ? <Minimize2 size={21} strokeWidth={1.6} /> : <Maximize2 size={21} strokeWidth={1.6} />}
          </ActionButton>
          <a
            href="/api/screenshot"
            title="Screenshot"
            aria-label="Screenshot"
            className={`holo-pressable flex h-14 w-14 items-center justify-center ${running ? "" : "pointer-events-none opacity-35"}`}
          >
            <Camera size={22} strokeWidth={1.6} />
          </a>
          <ActionButton label={running ? "Shut down" : "Power on"} active={running} onClick={() => void power(running ? "stop" : "start")}>
            <Power size={22} strokeWidth={1.6} />
          </ActionButton>
        </div>
      </header>

      <div className="flex min-h-0 flex-1 flex-col overflow-y-auto lg:flex-row lg:overflow-hidden">
        <main
          ref={stage}
          className="relative flex min-h-[70vh] flex-1 items-center justify-center overflow-auto lg:min-h-0"
        >
          <div
            ref={focusTarget}
            tabIndex={0}
            role="application"
            aria-label="Emulated HD2. Click the screen to touch; keyboard shortcuts and typing go to the phone."
            className="outline-none"
            onKeyDown={(e) => onStageKey(e, true)}
            onKeyUp={(e) => onStageKey(e, false)}
            onPointerDown={() => focusTarget.current?.focus({ preventScroll: true })}
          >
            {frameW > 0 && (
              <DeviceFrame
                width={frameW}
                canvasRef={canvas}
                send={send}
                interactive={running}
                overlay={overlay}
                pressed={pressed}
                onKey={onKey}
              />
            )}
          </div>
          {running && (
            <div className="pointer-events-none absolute bottom-3 left-4 font-mono text-[11px] text-holo-text-3">
              {video.mode === "h264" ? `${video.codec} · WebCodecs · decode ${video.decodeMs} ms` : video.mode === "jpeg" ? "JPEG fallback (no WebCodecs)" : ""}
            </div>
          )}
        </main>

        <aside className="flex min-h-[70vh] w-full shrink-0 flex-col border-holo-divider bg-holo-panel lg:h-full lg:min-h-0 lg:w-[420px] lg:border-l">
          <nav className="flex shrink-0 bg-[#141414]" role="tablist">
            {TABS.map((t, i) => (
              <button
                key={t}
                type="button"
                role="tab"
                aria-selected={tab === t}
                onClick={() => setTab(t)}
                className={`holo-pressable relative h-12 flex-1 text-[13.5px] font-medium uppercase tracking-[0.06em] ${
                  tab === t ? "text-holo-text" : "text-holo-text-2"
                } ${i ? "before:absolute before:top-3 before:bottom-3 before:left-0 before:w-px before:bg-white/10" : ""}`}
              >
                {t}
                <span className={`absolute inset-x-0 bottom-0 ${tab === t ? "h-1 bg-holo-blue" : "h-px bg-holo-blue/35"}`} />
              </button>
            ))}
          </nav>
          <div className="min-h-0 flex-1 overflow-y-auto">
            {tab === "Device" && (
              <DevicePanel status={status} pressed={pressed} onKey={onKey} onPower={power} onType={typeText} video={video} />
            )}
            {tab === "Config" && config && server && (
              <ConfigPanel config={config} images={server.images} firmware={server.firmware} onChange={saveConfig} />
            )}
            {tab === "Sensors" && <SensorsPanel status={status} post={postSensors} />}
            {tab === "Logs" && <LogsPanel active={tab === "Logs"} />}
          </div>
        </aside>
      </div>

      <Dialog
        open={restartAsk}
        title="Restart to apply?"
        onClose={() => setRestartAsk(false)}
        buttons={[
          { label: "Later", onClick: () => setRestartAsk(false) },
          {
            label: "Restart now",
            onClick: () => {
              setRestartAsk(false);
              void power("restart");
            },
          },
        ]}
      >
        <p className="px-4 py-5 text-[15px] leading-6 text-holo-text-2">
          The emulator is running with the previous settings. Restarting QEMU applies the new ones; Android boots again from the start.
        </p>
      </Dialog>
      <Toast text={toast} />
      {!connected && (
        <div className="fixed inset-x-0 top-14 z-30 flex justify-center">
          <div className="flex items-center gap-3 bg-[#2c2c2c] px-4 py-2 text-[13px] text-holo-text-2 shadow-lg">
            <Spinner size={16} /> Waiting for the web server
          </div>
        </div>
      )}
    </div>
  );
};

export default EmulatorShell;
