import type { HardKey } from "./emulator-types";

/* Positions in public/hd2-frame.webp (1165 x 2045), measured on the photo. */
export const FRAME = { w: 1165, h: 2045 };
export const SCREEN = { x: 110, y: 161.75, w: 941, h: 1568 };

export type Hotspot = { key: HardKey; x: number; y: number; w: number; h: number; label: string; side?: boolean };

export const HOTSPOTS: Hotspot[] = [
  { key: "call", x: 140, y: 1866, w: 160, h: 86, label: "Call" },
  { key: "home", x: 306, y: 1866, w: 164, h: 86, label: "Home" },
  { key: "menu", x: 478, y: 1866, w: 172, h: 86, label: "Windows (Menu)" },
  { key: "back", x: 650, y: 1866, w: 190, h: 86, label: "Back" },
  { key: "end", x: 856, y: 1866, w: 154, h: 86, label: "End call" },
  { key: "volup", x: 0, y: 573, w: 52, h: 196, label: "Volume up", side: true },
  { key: "voldown", x: 0, y: 769, w: 52, h: 196, label: "Volume down", side: true },
];

/* The power key sits on the top edge, at the right, so the front photo cannot show it. */
export const POWER = { x: 820, y: 2, w: 150, h: 30 };

export const pct = (v: number, of: number) => `${(v / of) * 100}%`;
