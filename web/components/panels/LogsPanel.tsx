"use client";

import { useEffect, useRef, useState } from "react";
import { Checkbox, ListPref } from "@/components/holo";
import type { LogName } from "@/lib/emulator-types";

const LOGS: { value: LogName; label: string; summary: string }[] = [
  { value: "ramcon", label: "Kernel console", summary: "Linux printk, from the RAM console" },
  { value: "uart1", label: "UART1", summary: "cLK and AMSS fatal errors" },
  { value: "qemu", label: "QEMU (ARM)", summary: "qemu-system-arm output" },
  { value: "q6", label: "QEMU (Hexagon)", summary: "qemu-system-hexagon output" },
];

export const LogsPanel = ({ active }: { active: boolean }) => {
  const [name, setName] = useState<LogName>("ramcon");
  const [text, setText] = useState("");
  const [follow, setFollow] = useState(true);
  const pre = useRef<HTMLPreElement>(null);

  useEffect(() => {
    if (!active) return;
    let stop = false;
    const load = async () => {
      const r = await fetch(`/api/log?name=${name}`, { cache: "no-store" }).catch(() => null);
      if (!stop && r?.ok) setText(await r.text());
    };
    void load();
    const iv = setInterval(load, 2000);
    return () => {
      stop = true;
      clearInterval(iv);
    };
  }, [name, active]);

  useEffect(() => {
    if (follow && pre.current) pre.current.scrollTop = pre.current.scrollHeight;
  }, [text, follow]);

  return (
    <div className="flex h-full flex-col">
      <ListPref title="Log" value={name} choices={LOGS} onChange={setName} />
      <label className="flex items-center gap-2 px-4 pb-2 text-[14px] text-holo-text-2">
        <Checkbox checked={follow} onChange={setFollow} /> Follow the end
      </label>
      <pre
        ref={pre}
        className="mx-4 mb-4 min-h-0 flex-1 overflow-auto border border-holo-divider bg-black/70 p-3 font-mono text-[11.5px] leading-[1.45] text-[#c8e6c9]"
      >
        {text || "Empty."}
      </pre>
    </div>
  );
};
