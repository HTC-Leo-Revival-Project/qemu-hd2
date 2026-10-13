"use client";

import { useCallback, useEffect, useRef, useState, type ReactNode } from "react";
import { FRAME, HOTSPOTS, POWER, SCREEN, pct } from "@/lib/frame-geometry";
import type { HardKey, InputMessage } from "@/lib/emulator-types";

type Props = {
  width: number;
  canvasRef: React.RefObject<HTMLCanvasElement | null>;
  send: (m: InputMessage) => void;
  interactive: boolean;
  overlay?: ReactNode;
  pressed: Set<HardKey>;
  onKey: (k: HardKey, down: boolean) => void;
};

export const DeviceFrame = ({ width, canvasRef, send, interactive, overlay, pressed, onKey }: Props) => {
  const down = useRef(false);
  const pending = useRef<{ x: number; y: number } | null>(null);
  const raf = useRef(0);
  const [touchAt, setTouchAt] = useState<{ x: number; y: number } | null>(null);

  const norm = (e: React.PointerEvent<HTMLCanvasElement>) => {
    const r = e.currentTarget.getBoundingClientRect();
    return { x: (e.clientX - r.left) / r.width, y: (e.clientY - r.top) / r.height };
  };

  const flush = useCallback(() => {
    raf.current = 0;
    const p = pending.current;
    pending.current = null;
    if (p && down.current) send({ t: "touch", a: "move", ...p });
  }, [send]);

  useEffect(() => () => cancelAnimationFrame(raf.current), []);

  const onDown = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (!interactive || e.button > 0) return;
    e.currentTarget.setPointerCapture(e.pointerId);
    down.current = true;
    const p = norm(e);
    setTouchAt(p);
    send({ t: "touch", a: "down", ...p });
  };
  const onMove = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (!down.current) return;
    const p = norm(e);
    pending.current = p;
    setTouchAt(p);
    if (!raf.current) raf.current = requestAnimationFrame(flush);
  };
  const onUp = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (!down.current) return;
    down.current = false;
    cancelAnimationFrame(raf.current);
    raf.current = 0;
    pending.current = null;
    setTouchAt(null);
    send({ t: "touch", a: "up", ...norm(e) });
  };

  const keyHandlers = (k: HardKey) => ({
    onPointerDown: (e: React.PointerEvent) => {
      e.preventDefault();
      (e.currentTarget as HTMLElement).setPointerCapture(e.pointerId);
      onKey(k, true);
    },
    onPointerUp: () => onKey(k, false),
    onPointerCancel: () => onKey(k, false),
    onLostPointerCapture: () => pressed.has(k) && onKey(k, false),
  });

  return (
    <div className="relative select-none" style={{ width, height: (width * FRAME.h) / FRAME.w }}>
      <button
        type="button"
        aria-label="Power"
        title="Power (F12)"
        data-down={pressed.has("power")}
        className="absolute z-0 rounded-t-[6px] bg-gradient-to-b from-[#3a3a3a] to-[#111] shadow-[inset_0_1px_0_rgba(255,255,255,0.18)] transition-transform data-[down=true]:translate-y-[2px] data-[down=true]:shadow-[0_0_14px_rgba(51,181,229,0.6)]"
        style={{ left: pct(POWER.x, FRAME.w), top: pct(POWER.y, FRAME.h), width: pct(POWER.w, FRAME.w), height: pct(POWER.h, FRAME.h) }}
        {...keyHandlers("power")}
      />
      <canvas
        ref={canvasRef}
        width={480}
        height={800}
        className={`absolute z-[1] touch-none bg-black ${interactive ? "cursor-crosshair" : ""}`}
        style={{
          left: pct(SCREEN.x - 2, FRAME.w),
          top: pct(SCREEN.y - 2, FRAME.h),
          width: pct(SCREEN.w + 4, FRAME.w),
          height: pct(SCREEN.h + 4, FRAME.h),
        }}
        onPointerDown={onDown}
        onPointerMove={onMove}
        onPointerUp={onUp}
        onPointerCancel={onUp}
        onContextMenu={(e) => e.preventDefault()}
      />
      {touchAt && (
        <span
          className="pointer-events-none absolute z-[3] size-9 -translate-x-1/2 -translate-y-1/2 rounded-full border-2 border-holo-blue/80 bg-holo-blue/20"
          style={{
            left: `calc(${pct(SCREEN.x, FRAME.w)} + ${touchAt.x} * ${pct(SCREEN.w, FRAME.w)})`,
            top: `calc(${pct(SCREEN.y, FRAME.h)} + ${touchAt.y} * ${pct(SCREEN.h, FRAME.h)})`,
          }}
        />
      )}
      {overlay && (
        <div
          className="absolute z-[2] flex items-center justify-center"
          style={{ left: pct(SCREEN.x, FRAME.w), top: pct(SCREEN.y, FRAME.h), width: pct(SCREEN.w, FRAME.w), height: pct(SCREEN.h, FRAME.h) }}
        >
          {overlay}
        </div>
      )}
      {/* eslint-disable-next-line @next/next/no-img-element */}
      <img
        src="/hd2-frame.webp"
        alt="HTC HD2"
        draggable={false}
        className="pointer-events-none absolute inset-0 z-[4] h-full w-full"
      />
      {HOTSPOTS.map((h) => (
        <button
          key={h.key}
          type="button"
          aria-label={h.label}
          title={h.label}
          data-down={pressed.has(h.key)}
          className={`hd2-key absolute z-[5] touch-none ${h.side ? "rounded-[6px]" : ""}`}
          style={{ left: pct(h.x, FRAME.w), top: pct(h.y, FRAME.h), width: pct(h.w, FRAME.w), height: pct(h.h, FRAME.h) }}
          {...keyHandlers(h.key)}
        />
      ))}
    </div>
  );
};
