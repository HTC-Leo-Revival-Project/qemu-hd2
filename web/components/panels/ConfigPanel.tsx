"use client";

import { Cpu, Gauge, HardDrive, Headphones, Layers, MemoryStick, Radio, Terminal, Wifi, Signal, Usb, Film, Sparkles, Bug } from "lucide-react";
import { Category, Checkbox, Divider, EditTextPref, ListPref, PrefRow, SeekPref, Switch, type Choice } from "@/components/holo";
import type { EmulatorConfig, Firmware, ImageFile } from "@/lib/emulator-types";

const gb = (n: number) => (n >= 1 << 30 ? `${(n / (1 << 30)).toFixed(1)} GB` : `${Math.round(n / (1 << 20))} MB`);
const I = { size: 20, strokeWidth: 1.6 };

type Props = {
  config: EmulatorConfig;
  images: ImageFile[];
  firmware: Firmware;
  onChange: (patch: Partial<EmulatorConfig>) => void;
};

export const ConfigPanel = ({ config, images, firmware, onChange }: Props) => {
  const nands = images.filter((i) => i.kind === "nand");
  const sds = images.filter((i) => i.kind === "sdcard");

  const setBaseband = (baseband: EmulatorConfig["baseband"]) => {
    const patch: Partial<EmulatorConfig> = { baseband };
    const has = (n: string) => nands.some((i) => i.name === n);
    if (baseband === "lle" && config.nand === "nand.img" && has("nand-lle.img")) patch.nand = "nand-lle.img";
    if (baseband === "hle") {
      patch.dsp = "hle";
      if (config.nand === "nand-lle.img" && has("nand.img")) patch.nand = "nand.img";
    }
    onChange(patch);
  };

  const basebandChoices: Choice<EmulatorConfig["baseband"]>[] = [
    { value: "hle", label: "Modelled (HLE)", summary: "Fast. Network registration, data, SMS and GPS fixes work." },
    {
      value: "lle",
      label: "Real AMSS firmware (LLE)",
      summary: firmware.amss ? "The radio ROM's own ARM9 code. No network signal." : "Needs images/amss.elf",
      disabled: !firmware.amss,
    },
  ];
  const dspChoices: Choice<EmulatorConfig["dsp"]>[] = [
    { value: "hle", label: "Not running", summary: "Codecs and audio fall back to software" },
    {
      value: "lle",
      label: "Real DSP1 firmware (LLE)",
      summary: firmware.dsp1 && firmware.hexagon ? "QDSP6 in qemu-system-hexagon: H.264 decode, MPEG-4 encode, audio" : "Needs images/dsp1.elf and qemu-system-hexagon",
      disabled: !firmware.dsp1 || !firmware.hexagon,
    },
  ];

  return (
    <div className="pb-8">
      <Category>Processors</Category>
      <ListPref title="Baseband (modem core)" icon={<Radio {...I} />} value={config.baseband} choices={basebandChoices} onChange={setBaseband} />
      <Divider />
      <ListPref
        title="QDSP6 (multimedia DSP)"
        icon={<Cpu {...I} />}
        value={config.dsp}
        choices={dspChoices}
        disabled={config.baseband !== "lle"}
        onChange={(dsp) => onChange({ dsp })}
      />
      {config.baseband !== "lle" && <p className="px-4 pb-2 pl-16 text-[12.5px] text-holo-text-3">The DSP firmware runs on top of the real baseband.</p>}
      {config.dsp === "lle" && (
        <>
          <Divider />
          <ListPref
            title="DSP clock (icount shift)"
            icon={<Gauge {...I} />}
            value={String(config.dspShift) as "0" | "1" | "2" | "3" | "4"}
            choices={[
              { value: "0", label: "shift=0 (1 ns per instruction)" },
              { value: "1", label: "shift=1 (default)" },
              { value: "2", label: "shift=2" },
              { value: "3", label: "shift=3" },
              { value: "4", label: "shift=4" },
            ]}
            onChange={(v) => onChange({ dspShift: Number(v) })}
          />
        </>
      )}

      <Category>Images</Category>
      <ListPref
        title="NAND flash"
        icon={<MemoryStick {...I} />}
        value={config.nand}
        choices={nands.map((n) => ({ value: n.name, label: n.name, summary: gb(n.size) }))}
        onChange={(nand) => onChange({ nand })}
      />
      <Divider />
      <ListPref
        title="microSD card"
        icon={<HardDrive {...I} />}
        value={config.sdcard}
        choices={[{ value: "", label: "No card" }, ...sds.map((n) => ({ value: n.name, label: n.name, summary: gb(n.size) }))]}
        onChange={(sdcard) => onChange({ sdcard })}
      />
      <Divider />
      <ListPref
        title="Boot"
        icon={<Layers {...I} />}
        value={config.boot}
        choices={[
          { value: "nand", label: "Boot from NAND", summary: "Picks BOOT FROM NAND in the cLK menu" },
          { value: "recovery", label: "Recovery", summary: "Picks RECOVERY in the cLK menu" },
          { value: "menu", label: "Stay in the bootloader", summary: "No key presses; also for MAGLDR images" },
        ]}
        onChange={(boot) => onChange({ boot })}
      />

      <Category>Graphics and sound</Category>
      <ListPref
        title="Adreno 200 renderer"
        icon={<Sparkles {...I} />}
        value={config.gpu}
        choices={[
          { value: "gl", label: "Host GPU (OpenGL 4.5)", summary: "Shaders translated to GLSL" },
          { value: "soft", label: "Software", summary: "Shaders interpreted on the CPU" },
        ]}
        onChange={(gpu) => onChange({ gpu })}
      />
      <Divider />
      <ListPref
        title="Audio output"
        icon={<Headphones {...I} />}
        value={config.audio}
        choices={[
          { value: "browser", label: "This browser", summary: "Streamed as PCM next to the picture" },
          { value: "host", label: "Host speakers", summary: "PipeWire on the machine running QEMU" },
          { value: "both", label: "Browser and host" },
          { value: "off", label: "Muted" },
        ]}
        onChange={(audio) => onChange({ audio })}
      />

      <Category>Network</Category>
      <PrefRow title="WiFi" summary="Open network HD2-QEMU with internet through QEMU" icon={<Wifi {...I} />} widget={<Switch checked={config.wifi} onChange={(wifi) => onChange({ wifi })} />} />
      <Divider />
      <PrefRow
        title="Mobile data"
        summary="The modem's packet data link"
        icon={<Signal {...I} />}
        widget={<Switch checked={config.mobileData} onChange={(mobileData) => onChange({ mobileData })} />}
      />
      <Divider />
      <EditTextPref
        title="adb port"
        icon={<Usb {...I} />}
        value={String(config.adbPort)}
        summary={`adb connect 127.0.0.1:${config.adbPort}`}
        inputMode="numeric"
        onChange={(v) => onChange({ adbPort: Number(v) || 5555 })}
      />

      <Category>Screen stream</Category>
      <ListPref
        title="Encoder"
        icon={<Film {...I} />}
        value={config.encoder}
        choices={[
          { value: "x264", label: "x264 (CPU)", summary: "Lowest latency at this size" },
          { value: "nvenc", label: "NVENC (NVIDIA GPU)" },
        ]}
        onChange={(encoder) => onChange({ encoder })}
      />
      <SeekPref
        title="Picture quality"
        value={50 - config.quality}
        min={10}
        max={40}
        format={(v) => `CRF ${50 - v}`}
        onCommit={(v) => onChange({ quality: 50 - v })}
      />
      <SeekPref title="Frame rate limit" value={config.maxFps} min={10} max={60} step={5} format={(v) => `${v} fps`} onCommit={(maxFps) => onChange({ maxFps })} />
      <p className="px-4 text-[12.5px] text-holo-text-3">Stream settings apply at once. The others take effect when the emulator restarts.</p>

      <Category>Debugging</Category>
      <PrefRow
        title="AMSS debug log"
        summary="The baseband's F3 messages in images/qemu.log (LLE)"
        icon={<Bug {...I} />}
        disabled={config.baseband !== "lle"}
        widget={<Checkbox checked={config.amssLog} onChange={(amssLog) => onChange({ amssLog })} />}
      />
      <Divider />
      <EditTextPref
        title="Extra QEMU arguments"
        icon={<Terminal {...I} />}
        value={config.extraArgs}
        placeholder="None"
        onChange={(extraArgs) => onChange({ extraArgs })}
      />
    </div>
  );
};
