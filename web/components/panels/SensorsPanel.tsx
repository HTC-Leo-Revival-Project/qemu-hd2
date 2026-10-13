"use client";

import { useState } from "react";
import { BatteryMedium, MapPin, PlugZap } from "lucide-react";
import { Category, HoloButton, ListPref, SeekPref } from "@/components/holo";
import type { Cable, EmulatorStatus } from "@/lib/emulator-types";

const PLACES = [
  { name: "Googleplex", lat: 37.422, lon: -122.0841, alt: 10 },
  { name: "São Paulo", lat: -23.5505, lon: -46.6333, alt: 760 },
  { name: "London", lat: 51.5074, lon: -0.1278, alt: 11 },
  { name: "Tokyo", lat: 35.6762, lon: 139.6503, alt: 40 },
];

const I = { size: 20, strokeWidth: 1.6 };

type Props = {
  status: EmulatorStatus | null;
  post: (body: object, done: string) => Promise<void>;
};

export const SensorsPanel = ({ status, post }: Props) => {
  const on = status?.state === "running";
  const lle = status?.running?.baseband === "lle";
  const [gps, setGps] = useState({ lat: "37.4220", lon: "-122.0841", alt: "10" });

  return (
    <div className="pb-8">
      <Category>Power</Category>
      <SeekPref
        title="Battery level"
        icon={<BatteryMedium {...I} />}
        value={status?.sensors.battery ?? 100}
        min={0}
        max={100}
        format={(v) => `${v}%`}
        disabled={!on}
        onCommit={(battery) => void post({ battery }, `Battery set to ${battery}%`)}
      />
      <ListPref
        title="Charger"
        icon={<PlugZap {...I} />}
        value={(status?.sensors.cable ?? "usb") as Cable}
        disabled={!on}
        choices={[
          { value: "usb", label: "USB cable (adb)", summary: "Charges slowly" },
          { value: "ac", label: "Wall charger", summary: "Fast charge" },
          { value: "none", label: "Unplugged", summary: "The battery drains" },
        ]}
        onChange={(cable) => void post({ cable }, cable === "none" ? "Charger unplugged" : "Charger plugged in")}
      />
      <p className="px-4 pl-16 text-[12.5px] text-holo-text-3">The DS2746 gauge charges and drains in real time, like the 1230 mAh battery.</p>

      <Category>Location</Category>
      <div className={`px-4 pt-2 ${!on || lle ? "pointer-events-none opacity-40" : ""}`}>
        <div className="flex items-center gap-4 pb-2 text-holo-text-2">
          <MapPin {...I} />
          <span className="text-[17px] text-holo-text">GPS fix</span>
        </div>
        <div className="grid grid-cols-3 gap-4 pl-12">
          {(["lat", "lon", "alt"] as const).map((k) => (
            <label key={k} className="block">
              <span className="text-[11px] uppercase tracking-[0.08em] text-holo-text-3">
                {k === "lat" ? "Latitude" : k === "lon" ? "Longitude" : "Altitude m"}
              </span>
              <input
                className="holo-field w-full font-mono text-[14px]"
                inputMode="decimal"
                value={gps[k]}
                onChange={(e) => setGps({ ...gps, [k]: e.target.value })}
              />
            </label>
          ))}
        </div>
        <div className="flex flex-wrap gap-2 pt-4 pl-12">
          {PLACES.map((p) => (
            <button
              key={p.name}
              type="button"
              className="holo-pressable border border-white/10 px-3 py-1.5 text-[13px] text-holo-text-2"
              onClick={() => setGps({ lat: String(p.lat), lon: String(p.lon), alt: String(p.alt) })}
            >
              {p.name}
            </button>
          ))}
        </div>
        <div className="pt-4 pl-12">
          <HoloButton
            primary
            onClick={() =>
              void post({ gps: { lat: Number(gps.lat), lon: Number(gps.lon), alt: Number(gps.alt) } }, `GPS fix sent: ${gps.lat}, ${gps.lon}`)
            }
          >
            Send fix
          </HoloButton>
        </div>
      </div>
      <p className="px-4 pt-3 pl-16 text-[12.5px] text-holo-text-3">
        {lle
          ? "With the real baseband, gpsOne finds no satellites (there is no RF signal), so fixes come only from the modelled baseband."
          : "Location requests get a 1 Hz fix with eight satellites in view."}
      </p>
    </div>
  );
};
