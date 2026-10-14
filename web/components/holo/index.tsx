"use client";

import { useEffect, useId, useRef, useState, type ReactNode } from "react";

export const Category = ({ children }: { children: ReactNode }) => (
  <div className="px-4 pt-5 pb-1.5">
    <div className="text-[13px] font-bold uppercase tracking-[0.04em] text-holo-blue">{children}</div>
    <div className="mt-1.5 h-px bg-holo-blue/45" />
  </div>
);

export const Divider = () => <div className="mx-4 h-px bg-holo-divider" />;

type RowProps = {
  title: ReactNode;
  summary?: ReactNode;
  icon?: ReactNode;
  widget?: ReactNode;
  onClick?: () => void;
  disabled?: boolean;
};

export const PrefRow = ({ title, summary, icon, widget, onClick, disabled }: RowProps) => {
  const body = (
    <>
      {icon && <span className="flex size-8 shrink-0 items-center justify-center text-holo-text-2">{icon}</span>}
      <span className="min-w-0 flex-1">
        <span className="block truncate text-[17px] leading-6 text-holo-text">{title}</span>
        {summary && <span className="mt-0.5 block text-[13.5px] leading-5 text-holo-text-3">{summary}</span>}
      </span>
      {widget && <span className="shrink-0">{widget}</span>}
    </>
  );
  const cls = `flex min-h-[64px] w-full items-center gap-4 px-4 py-2.5 text-left ${disabled ? "pointer-events-none opacity-40" : ""}`;
  return onClick ? (
    <button type="button" className={`${cls} holo-pressable`} onClick={onClick} disabled={disabled}>
      {body}
    </button>
  ) : (
    <div className={cls}>{body}</div>
  );
};

export const Switch = ({ checked, onChange, disabled }: { checked: boolean; onChange: (v: boolean) => void; disabled?: boolean }) => (
  <button
    type="button"
    role="switch"
    aria-checked={checked}
    disabled={disabled}
    onClick={(e) => {
      e.stopPropagation();
      onChange(!checked);
    }}
    className="relative h-[30px] w-[92px] shrink-0 bg-[#2b2b2b] shadow-[inset_0_1px_3px_rgba(0,0,0,0.8)] disabled:opacity-40"
  >
    <span
      className={`absolute top-[3px] flex h-6 w-[44px] items-center justify-center text-[12px] font-bold tracking-wide transition-all duration-150 ${
        checked
          ? "left-[45px] bg-gradient-to-b from-[#3fc0ee] to-[#0099cc] text-white"
          : "left-[3px] bg-gradient-to-b from-[#6a6a6a] to-[#474747] text-[#dcdcdc]"
      }`}
    >
      {checked ? "ON" : "OFF"}
    </span>
  </button>
);

export const Checkbox = ({ checked, onChange, disabled }: { checked: boolean; onChange: (v: boolean) => void; disabled?: boolean }) => (
  <button
    type="button"
    role="checkbox"
    aria-checked={checked}
    disabled={disabled}
    onClick={(e) => {
      e.stopPropagation();
      onChange(!checked);
    }}
    className="relative flex size-8 items-center justify-center disabled:opacity-40"
  >
    <span className="block size-[20px] border-2 border-[#9a9a9a] bg-black/30" />
    {checked && (
      <svg viewBox="0 0 24 24" className="absolute size-7 text-holo-blue" aria-hidden>
        <path d="M5 12.5 10 17.5 21 5" fill="none" stroke="currentColor" strokeWidth="3.2" strokeLinecap="square" />
      </svg>
    )}
  </button>
);

const Radio = ({ checked }: { checked: boolean }) => (
  <span className="relative flex size-[22px] items-center justify-center rounded-full border-2 border-[#9a9a9a]">
    {checked && <span className="size-[11px] rounded-full bg-holo-blue shadow-[0_0_8px_rgba(51,181,229,0.8)]" />}
  </span>
);

export const HoloButton = ({
  children,
  onClick,
  disabled,
  primary,
  className = "",
  title,
  onPointerDown,
  onPointerUp,
  type = "button",
}: {
  children: ReactNode;
  onClick?: () => void;
  disabled?: boolean;
  primary?: boolean;
  className?: string;
  title?: string;
  onPointerDown?: () => void;
  onPointerUp?: () => void;
  type?: "button" | "submit";
}) => (
  <button
    type={type}
    title={title}
    onClick={onClick}
    onPointerDown={onPointerDown}
    onPointerUp={onPointerUp}
    onPointerLeave={onPointerUp}
    disabled={disabled}
    className={`inline-flex min-h-12 select-none items-center justify-center gap-2 px-4 text-[15px] text-holo-text transition-colors duration-100 disabled:opacity-35 ${
      primary
        ? "border border-holo-blue/60 bg-holo-blue/20 hover:bg-holo-blue/30 active:bg-holo-blue/60"
        : "border border-white/10 bg-white/[0.07] hover:bg-white/[0.11] active:bg-[var(--holo-pressed)]"
    } shadow-[inset_0_1px_0_rgba(255,255,255,0.06)] ${className}`}
  >
    {children}
  </button>
);

export const Dialog = ({
  open,
  title,
  children,
  onClose,
  buttons,
}: {
  open: boolean;
  title: ReactNode;
  children: ReactNode;
  onClose: () => void;
  buttons?: { label: string; onClick: () => void; disabled?: boolean }[];
}) => {
  useEffect(() => {
    if (!open) return;
    const k = (e: KeyboardEvent) => e.key === "Escape" && onClose();
    window.addEventListener("keydown", k);
    return () => window.removeEventListener("keydown", k);
  }, [open, onClose]);
  if (!open) return null;
  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-4" onPointerDown={onClose}>
      <div
        role="dialog"
        aria-modal
        className="w-full max-w-[420px] bg-holo-dialog shadow-[0_8px_40px_rgba(0,0,0,0.8)] [animation:holo-dialog-in_140ms_ease-out]"
        onPointerDown={(e) => e.stopPropagation()}
      >
        <div className="px-4 pt-4 pb-3 text-[20px] text-holo-blue">{title}</div>
        <div className="h-[2px] bg-holo-blue" />
        <div className="max-h-[60vh] overflow-y-auto">{children}</div>
        {buttons && buttons.length > 0 && (
          <div className="flex border-t border-holo-divider">
            {buttons.map((b, i) => (
              <button
                key={b.label}
                type="button"
                disabled={b.disabled}
                onClick={b.onClick}
                className={`holo-pressable min-h-12 flex-1 text-[15px] disabled:opacity-35 ${i > 0 ? "border-l border-holo-divider" : ""}`}
              >
                {b.label}
              </button>
            ))}
          </div>
        )}
      </div>
    </div>
  );
};

export type Choice<T extends string> = { value: T; label: string; summary?: string; disabled?: boolean };

export const ListPref = <T extends string>({
  title,
  value,
  choices,
  onChange,
  icon,
  disabled,
}: {
  title: string;
  value: T;
  choices: Choice<T>[];
  onChange: (v: T) => void;
  icon?: ReactNode;
  disabled?: boolean;
}) => {
  const [open, setOpen] = useState(false);
  const current = choices.find((c) => c.value === value);
  return (
    <>
      <PrefRow title={title} summary={current?.label ?? value} icon={icon} disabled={disabled} onClick={() => setOpen(true)} />
      <Dialog open={open} title={title} onClose={() => setOpen(false)} buttons={[{ label: "Cancel", onClick: () => setOpen(false) }]}>
        {choices.map((c) => (
          <button
            key={c.value}
            type="button"
            disabled={c.disabled}
            className="holo-pressable flex min-h-14 w-full items-center gap-4 border-b border-holo-divider px-4 py-2 text-left disabled:opacity-35"
            onClick={() => {
              onChange(c.value);
              setOpen(false);
            }}
          >
            <span className="min-w-0 flex-1">
              <span className="block text-[17px]">{c.label}</span>
              {c.summary && <span className="block text-[13px] text-holo-text-3">{c.summary}</span>}
            </span>
            <Radio checked={c.value === value} />
          </button>
        ))}
      </Dialog>
    </>
  );
};

export const EditTextPref = ({
  title,
  value,
  onChange,
  summary,
  placeholder,
  icon,
  inputMode,
  disabled,
}: {
  title: string;
  value: string;
  onChange: (v: string) => void;
  summary?: string;
  placeholder?: string;
  icon?: ReactNode;
  inputMode?: "text" | "numeric";
  disabled?: boolean;
}) => {
  const [open, setOpen] = useState(false);
  const [draft, setDraft] = useState(value);
  const id = useId();
  const ok = () => {
    onChange(draft);
    setOpen(false);
  };
  return (
    <>
      <PrefRow
        title={title}
        summary={summary ?? (value || placeholder)}
        icon={icon}
        disabled={disabled}
        onClick={() => {
          setDraft(value);
          setOpen(true);
        }}
      />
      <Dialog
        open={open}
        title={title}
        onClose={() => setOpen(false)}
        buttons={[
          { label: "Cancel", onClick: () => setOpen(false) },
          { label: "OK", onClick: ok },
        ]}
      >
        <form
          className="px-4 py-5"
          onSubmit={(e) => {
            e.preventDefault();
            ok();
          }}
        >
          <label htmlFor={id} className="sr-only">
            {title}
          </label>
          <input
            id={id}
            autoFocus
            inputMode={inputMode}
            className="holo-field w-full font-mono text-[15px]"
            value={draft}
            placeholder={placeholder}
            onChange={(e) => setDraft(e.target.value)}
          />
        </form>
      </Dialog>
    </>
  );
};

export const SeekPref = ({
  title,
  value,
  min,
  max,
  step = 1,
  format,
  onCommit,
  icon,
  disabled,
}: {
  title: string;
  value: number;
  min: number;
  max: number;
  step?: number;
  format: (v: number) => string;
  onCommit: (v: number) => void;
  icon?: ReactNode;
  disabled?: boolean;
}) => {
  const [v, setV] = useState(value);
  const dragging = useRef(false);
  useEffect(() => {
    if (!dragging.current) setV(value);
  }, [value]);
  const pct = ((v - min) / (max - min)) * 100;
  const commit = () => {
    dragging.current = false;
    if (v !== value) onCommit(v);
  };
  return (
    <div className={`px-4 py-2.5 ${disabled ? "pointer-events-none opacity-40" : ""}`}>
      <div className="flex items-center gap-4">
        {icon && <span className="flex size-8 shrink-0 items-center justify-center text-holo-text-2">{icon}</span>}
        <span className="flex-1 text-[17px]">{title}</span>
        <span className="font-mono text-[13px] text-holo-blue">{format(v)}</span>
      </div>
      <input
        type="range"
        className="holo-seek"
        min={min}
        max={max}
        step={step}
        value={v}
        aria-label={title}
        style={{ ["--p" as string]: `${pct}%` }}
        onPointerDown={() => (dragging.current = true)}
        onChange={(e) => setV(Number(e.target.value))}
        onPointerUp={commit}
        onKeyUp={commit}
      />
    </div>
  );
};

export const Toast = ({ text }: { text: string | null }) =>
  text ? (
    <div className="pointer-events-none fixed bottom-8 left-1/2 z-[60] max-w-[90vw] -translate-x-1/2 rounded-[3px] border border-white/10 bg-[#2c2c2c]/95 px-5 py-3 text-[14px] text-white shadow-[0_4px_20px_rgba(0,0,0,0.6)] [animation:holo-toast-in_160ms_ease-out]">
      {text}
    </div>
  ) : null;

export const Spinner = ({ size = 40 }: { size?: number }) => (
  <div className="holo-spinner" style={{ width: size, height: size }} role="progressbar" aria-label="Working" />
);
