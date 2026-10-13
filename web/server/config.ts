import fs from "node:fs";
import path from "node:path";
import { CONFIG_FILE, IMAGES, QEMU_ARM, QEMU_HEXAGON } from "./paths.ts";
import { DEFAULT_CONFIG, type EmulatorConfig, type Firmware, type ImageFile } from "../lib/emulator-types.ts";

const oneOf = <T extends string>(v: unknown, opts: readonly T[], d: T): T =>
  typeof v === "string" && (opts as readonly string[]).includes(v) ? (v as T) : d;
const num = (v: unknown, lo: number, hi: number, d: number) =>
  typeof v === "number" && Number.isFinite(v) ? Math.min(hi, Math.max(lo, Math.round(v))) : d;
const bool = (v: unknown, d: boolean) => (typeof v === "boolean" ? v : d);
const imageName = (v: unknown, d: string) =>
  typeof v === "string" && (v === "" || /^[\w.-]+$/.test(v)) ? v : d;

export const sanitize = (raw: Partial<EmulatorConfig>, base: EmulatorConfig = DEFAULT_CONFIG): EmulatorConfig => {
  const c: EmulatorConfig = {
    baseband: oneOf(raw.baseband, ["hle", "lle"], base.baseband),
    dsp: oneOf(raw.dsp, ["hle", "lle"], base.dsp),
    nand: imageName(raw.nand, base.nand) || base.nand,
    sdcard: imageName(raw.sdcard, base.sdcard),
    boot: oneOf(raw.boot, ["nand", "recovery", "menu"], base.boot),
    gpu: oneOf(raw.gpu, ["gl", "soft"], base.gpu),
    audio: oneOf(raw.audio, ["browser", "host", "both", "off"], base.audio),
    wifi: bool(raw.wifi, base.wifi),
    mobileData: bool(raw.mobileData, base.mobileData),
    adbPort: num(raw.adbPort, 1024, 65535, base.adbPort),
    amssLog: bool(raw.amssLog, base.amssLog),
    dspShift: num(raw.dspShift, 0, 4, base.dspShift),
    extraArgs: typeof raw.extraArgs === "string" ? raw.extraArgs.slice(0, 2000) : base.extraArgs,
    encoder: oneOf(raw.encoder, ["x264", "nvenc"], base.encoder),
    quality: num(raw.quality, 10, 40, base.quality),
    maxFps: num(raw.maxFps, 10, 60, base.maxFps),
  };
  if (c.baseband === "hle") c.dsp = "hle";
  return c;
};

export const loadConfig = (): EmulatorConfig => {
  try {
    return sanitize(JSON.parse(fs.readFileSync(CONFIG_FILE, "utf8")));
  } catch {
    return { ...DEFAULT_CONFIG };
  }
};

export const saveConfig = (c: EmulatorConfig) => {
  fs.writeFileSync(CONFIG_FILE, JSON.stringify(c, null, 2) + "\n");
};

export const listImages = (): ImageFile[] => {
  let names: string[] = [];
  try {
    names = fs.readdirSync(IMAGES);
  } catch {
    return [];
  }
  return names
    .filter((n) => n.endsWith(".img"))
    .sort()
    .map((name) => {
      const size = fs.statSync(path.join(IMAGES, name)).size;
      return { name, size, kind: name.startsWith("sdcard") ? "sdcard" : "nand" } as ImageFile;
    });
};

const exists = (p: string) => {
  try {
    fs.accessSync(p);
    return true;
  } catch {
    return false;
  }
};

export const firmware = (): Firmware => ({
  amss: exists(path.join(IMAGES, "amss.elf")),
  dsp1: exists(path.join(IMAGES, "dsp1.elf")),
  arm: exists(QEMU_ARM),
  hexagon: exists(QEMU_HEXAGON),
});
