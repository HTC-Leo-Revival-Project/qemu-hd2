import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));

export const WEB_DIR = path.resolve(here, "..");
export const ROOT = path.resolve(WEB_DIR, "..");
export const IMAGES = path.join(ROOT, "images");
export const QEMU_BUILD = path.join(ROOT, "src", "qemu", "build");
export const QEMU_ARM = path.join(QEMU_BUILD, "qemu-system-arm");
export const QEMU_HEXAGON = path.join(QEMU_BUILD, "qemu-system-hexagon");
export const CONFIG_FILE = path.join(WEB_DIR, ".emulator.json");

export const SOCK = {
  qmp: path.join(IMAGES, "qmp.sock"),
  webqmp: path.join(IMAGES, "webqmp.sock"),
  q6webqmp: path.join(IMAGES, "q6webqmp.sock"),
  vnc: path.join(IMAGES, "vnc.sock"),
  q6qmp: path.join(IMAGES, "q6qmp.sock"),
  q6vnc: path.join(IMAGES, "q6vnc.sock"),
  q6link: path.join(IMAGES, "q6link.sock"),
};
