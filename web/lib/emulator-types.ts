export type Mode = "hle" | "lle";
export type BootTarget = "nand" | "recovery" | "menu";
export type AudioRoute = "browser" | "host" | "both" | "off";
export type GpuBackend = "gl" | "soft";
export type VideoEncoder = "x264" | "nvenc";
export type Cable = "usb" | "ac" | "none";

export type EmulatorConfig = {
  baseband: Mode;
  dsp: Mode;
  nand: string;
  sdcard: string;
  boot: BootTarget;
  gpu: GpuBackend;
  audio: AudioRoute;
  wifi: boolean;
  mobileData: boolean;
  adbPort: number;
  amssLog: boolean;
  dspShift: number;
  extraArgs: string;
  encoder: VideoEncoder;
  quality: number;
  maxFps: number;
};

export const DEFAULT_CONFIG: EmulatorConfig = {
  baseband: "hle",
  dsp: "hle",
  nand: "nand.img",
  sdcard: "sdcard.img",
  boot: "nand",
  gpu: "gl",
  audio: "browser",
  wifi: true,
  mobileData: true,
  adbPort: 5555,
  amssLog: false,
  dspShift: -1,
  extraArgs: "",
  encoder: "x264",
  quality: 20,
  maxFps: 60,
};

export type PowerState = "stopped" | "starting" | "running" | "stopping" | "crashed";

/* os: what a NAND image boots (a stock Windows Mobile ROM, or Android etc. through cLK/MAGLDR) */
export type NandOs = "winmo" | "other";
export type ImageFile = { name: string; size: number; kind: "nand" | "sdcard" | "firmware"; os?: NandOs };

export type Firmware = { amss: boolean; dsp1: boolean; arm: boolean; hexagon: boolean };

export type EmulatorStatus = {
  state: PowerState;
  since: number;
  /* the server's clock when it sent this (since is on that clock) */
  now: number;
  message: string;
  external: boolean;
  running: EmulatorConfig | null;
  pids: { arm?: number; hexagon?: number };
  stream: { fps: number; kbps: number; clients: number; width: number; height: number };
  sensors: { battery: number; cable: Cable };
};

export type ServerState = {
  config: EmulatorConfig;
  images: ImageFile[];
  firmware: Firmware;
  status: EmulatorStatus;
};

export type HardKey = "volup" | "voldown" | "menu" | "call" | "end" | "back" | "home" | "power";

export const HARD_KEY_QCODE: Record<HardKey, string> = {
  volup: "pgup",
  voldown: "pgdn",
  menu: "f1",
  call: "f2",
  end: "f3",
  back: "esc",
  home: "home",
  power: "f12",
};

export type InputMessage =
  | { t: "touch"; a: "down" | "move" | "up"; x: number; y: number }
  | { t: "key"; k: HardKey; d: boolean };

export type LogName = "uart1" | "ramcon" | "qemu" | "q6";
