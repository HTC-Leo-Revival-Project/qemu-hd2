import fs from "node:fs";
import net from "node:net";
import path from "node:path";
import { spawn, type ChildProcess } from "node:child_process";
import { EventEmitter } from "node:events";
import { IMAGES, QEMU_ARM, QEMU_BUILD, QEMU_HEXAGON, SOCK } from "./paths.ts";
import { nandOs } from "./config.ts";
import { Qmp } from "./qmp.ts";
import { VncClient } from "./vnc.ts";
import { StreamHub } from "./stream.ts";
import {
  HARD_KEY_QCODE,
  type Cable,
  type EmulatorConfig,
  type EmulatorStatus,
  type HardKey,
  type InputMessage,
  type PowerState,
} from "../lib/emulator-types.ts";

/*
 * Runs the emulated HD2 the way tools/start.sh and tools/start-q6.sh do, plus
 * a VNC socket per process that the web server reads the screen and the
 * audio from.  The QEMU processes get their own process group, so they keep
 * running when this server restarts; the next server adopts them through
 * images/qmp.sock and images/vnc.sock.
 */

const RUN_FILE = path.join(IMAGES, ".web-run.json");
const SCREEN_W = 480, SCREEN_H = 800;

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

const splitArgs = (s: string) =>
  [...s.matchAll(/"([^"]*)"|'([^']*)'|(\S+)/g)].map((m) => m[1] ?? m[2] ?? m[3]);

const canConnect = (p: string) =>
  new Promise<boolean>((resolve) => {
    const s = net.createConnection(p);
    const done = (ok: boolean) => {
      s.destroy();
      resolve(ok);
    };
    s.once("connect", () => done(true));
    s.once("error", () => done(false));
    setTimeout(() => done(false), 500);
  });

const waitFor = async (p: string, ms: number) => {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    if (fs.existsSync(p) && (await canConnect(p))) return true;
    await sleep(100);
  }
  return false;
};

const hostAudio = () => {
  const rt = process.env.XDG_RUNTIME_DIR || `/run/user/${process.getuid?.() ?? 1000}`;
  return fs.existsSync(path.join(rt, "pipewire-0")) ? "pipewire" : "none";
};

const tail = (file: string, n = 6) => {
  try {
    return fs.readFileSync(path.join(IMAGES, file), "utf8").trim().split("\n").slice(-n).join("\n");
  } catch {
    return "";
  }
};

export class Emulator extends EventEmitter {
  readonly hub: StreamHub;
  private arm: ChildProcess | null = null;
  private hexagon: ChildProcess | null = null;
  private qmp: Qmp | null = null;
  private q6qmp: Qmp | null = null;
  private vnc: VncClient | null = null;
  private q6vnc: VncClient | null = null;
  private state: PowerState = "stopped";
  private since = Date.now();
  private message = "";
  private external = false;
  private running: EmulatorConfig | null = null;
  private pids: { arm?: number; hexagon?: number } = {};
  private touch = { x: 0, y: 0, down: false };
  private sensors = { battery: 100, cable: "usb" as Cable };
  private op: Promise<void> = Promise.resolve();

  constructor(hub: StreamHub) {
    super();
    this.hub = hub;
  }

  status(): EmulatorStatus {
    return {
      state: this.state,
      since: this.since,
      now: Date.now(),
      message: this.message,
      external: this.external,
      running: this.running,
      pids: this.pids,
      stream: this.hub.statsSnapshot,
      sensors: this.sensors,
    };
  }

  private setState(s: PowerState, message = "") {
    if (s !== this.state) this.since = Date.now();
    this.state = s;
    this.message = message;
    this.emit("status");
  }

  /** Serialise power operations. */
  private queue(fn: () => Promise<void>) {
    const next = this.op.then(fn, fn);
    this.op = next.catch(() => {});
    return next;
  }

  /** A server that restarts finds the QEMU it started (or one from tools/start*.sh). */
  async adopt() {
    if (!(await canConnect(SOCK.qmp))) return;
    try {
      this.running = JSON.parse(fs.readFileSync(RUN_FILE, "utf8"));
    } catch {
      this.running = null;
    }
    this.external = this.running === null;
    await this.attach();
    this.setState(
      "running",
      this.vnc ? "" : "This emulator was started without the web UI's VNC socket, so there is no picture. Restart it from here.",
    );
  }

  start(cfg: EmulatorConfig) {
    return this.queue(async () => {
      if (this.state === "running" || this.state === "starting") throw new Error("The emulator is already running");
      if (await canConnect(SOCK.qmp)) {
        await this.adopt();
        throw new Error("Another emulator is already running; it has been attached. Stop it first to start a new one.");
      }
      this.setState("starting", "Launching QEMU");
      try {
        await this.launch(cfg);
        this.setState("running");
      } catch (e) {
        await this.killAll();
        this.setState("crashed", (e as Error).message);
        throw e;
      }
    });
  }

  stop() {
    return this.queue(async () => {
      if (this.state === "stopped") return;
      this.setState("stopping", "Shutting down");
      await this.killAll();
      this.setState("stopped");
    });
  }

  async restart(cfg: EmulatorConfig) {
    await this.stop();
    await this.start(cfg);
  }

  /** Hard reset of the board, like pulling the battery, without restarting QEMU. */
  async reset() {
    if (!this.qmp) throw new Error("The emulator is not running");
    await this.qmp.execute("system_reset");
    if (this.running && this.running.boot !== "menu") void this.bootKeys(this.running);
  }

  private async launch(cfg: EmulatorConfig) {
    /*
     * Windows Mobile boots straight from NAND (the machine finds the ROM's
     * XIP partition) and needs the real baseband; no cLK menu to drive
     */
    if (nandOs(cfg.nand) === "winmo") cfg = { ...cfg, baseband: "lle", boot: "menu" };
    const dsp = cfg.baseband === "lle" && cfg.dsp === "lle";
    for (const f of ["qmp.sock", "webqmp.sock", "vnc.sock", "q6vnc.sock", "q6webqmp.sock", "ramcon.log", "qemu.log"]) fs.rmSync(path.join(IMAGES, f), { force: true });
    if (!fs.existsSync(path.join(IMAGES, cfg.nand))) throw new Error(`NAND image ${cfg.nand} not found in images/`);
    if (cfg.baseband === "lle" && !fs.existsSync(path.join(IMAGES, "amss.elf"))) throw new Error("images/amss.elf is missing (tools/hd2_radio.py)");
    if (dsp && !fs.existsSync(path.join(IMAGES, "dsp1.elf"))) throw new Error("images/dsp1.elf is missing (tools/hd2_radio.py --dsp)");

    const toBrowser = cfg.audio === "browser" || cfg.audio === "both";
    const backend = cfg.audio === "host" || cfg.audio === "both" ? hostAudio() : "none";

    let machine = "htcleo";
    if (cfg.baseband === "lle") machine += ",baseband=lle,baseband-firmware=amss.elf,baseband-trace=off";
    const args = [
      "-M", machine,
      "-drive", `if=mtd,format=raw,file=${cfg.nand}`,
      "-display", "none",
      "-vnc", `unix:${SOCK.vnc}${toBrowser && !dsp ? ",audiodev=snd0" : ""}`,
      "-serial", "file:uart1.log", "-serial", "file:ramcon.log",
      "-qmp", `unix:${SOCK.qmp},server,nowait`,
      "-qmp", `unix:${SOCK.webqmp},server,nowait`,
      "-audiodev", `${backend},id=snd0`, "-global", "msm-modem.audiodev=snd0",
      "-global", `msm-adreno.backend=${cfg.gpu}`,
      "-chardev", `socket,id=adb,host=127.0.0.1,port=${cfg.adbPort},server=on,wait=off`, "-global", "msm-hsusb.adb=adb",
    ];
    let debug = "unimp,guest_errors";
    if (cfg.baseband === "lle" && cfg.amssLog) {
      args.push("-plugin", `${QEMU_BUILD}/contrib/plugins/libamsslog.so,msg=0x526604,msg=0xc55d88,msg=0xc57028`);
      debug += ",plugin";
    }
    args.push("-d", debug, "-D", "qemu.log");
    if (cfg.sdcard && fs.existsSync(path.join(IMAGES, cfg.sdcard))) args.push("-drive", `if=sd,format=raw,file=${cfg.sdcard}`);
    if (cfg.wifi) args.push("-nic", "user,model=bcm4329");
    if (cfg.mobileData) args.push("-nic", "user,model=msm-modem,net=10.0.3.0/24");

    const mem = ["-object", "memory-backend-file,id=htcleo.ram,size=1G,mem-path=dram.bin,share=on", "-machine", "memory-backend=htcleo.ram"];
    if (dsp) {
      for (const f of ["dram.bin", "q6link.sock", "q6.log", "q6qmp.sock"]) fs.rmSync(path.join(IMAGES, f), { force: true });
      fs.closeSync(fs.openSync(path.join(IMAGES, "dram.bin"), "w"));
      fs.truncateSync(path.join(IMAGES, "dram.bin"), 1 << 30);
      args.push(...mem, "-machine", "qdsp6=lle,qdsp6-link=q6link",
        "-chardev", `socket,id=q6link,path=${SOCK.q6link},server=on,wait=off`);
    }
    args.push(...splitArgs(cfg.extraArgs));

    this.arm = this.spawn(QEMU_ARM, args, "qemu.out", "arm");
    this.running = cfg;
    this.external = false;
    fs.writeFileSync(RUN_FILE, JSON.stringify(cfg));

    if (!(await waitFor(SOCK.webqmp, 8000))) throw new Error(`QEMU did not start:\n${tail("qemu.out")}`);

    if (dsp) {
      if (!(await waitFor(SOCK.q6link, 6000))) throw new Error("The DSP link socket did not appear");
      const q6 = [
        "-M", `htcleo-q6,link=q6link,apps-irq=11,modem-irq=2,trace=off`,
        "-kernel", "dsp1.elf",
        "-icount", `shift=${cfg.dspShift < 0 ? "auto" : cfg.dspShift}`,
        ...mem,
        "-chardev", `socket,id=q6link,path=${SOCK.q6link}`,
        "-audiodev", `${backend},id=q6snd`, "-global", "htcleo-q6-audif.audiodev=q6snd",
        "-display", "none",
        "-vnc", `unix:${SOCK.q6vnc}${toBrowser ? ",audiodev=q6snd" : ""}`,
        "-qmp", `unix:${SOCK.q6qmp},server,nowait`,
        "-qmp", `unix:${SOCK.q6webqmp},server,nowait`,
        "-d", "guest_errors,unimp", "-D", "q6.log",
        ...splitArgs(process.env.Q6_ARGS ?? ""),
      ];
      this.hexagon = this.spawn(QEMU_HEXAGON, q6, "q6.out", "hexagon");
    }

    await this.attach();
    if (cfg.boot !== "menu") void this.bootKeys(cfg);
  }

  private spawn(bin: string, args: string[], out: string, which: "arm" | "hexagon") {
    const fd = fs.openSync(path.join(IMAGES, out), "w");
    const child = spawn(bin, args, { cwd: IMAGES, stdio: ["ignore", fd, fd], detached: true });
    fs.closeSync(fd);
    this.pids[which] = child.pid;
    child.on("exit", (code, signal) => this.onExit(which, child, code, signal));
    return child;
  }

  private onExit(which: "arm" | "hexagon", child: ChildProcess, code: number | null, signal: NodeJS.Signals | null) {
    if (which === "arm" && this.arm === child) this.arm = null;
    if (which === "hexagon" && this.hexagon === child) this.hexagon = null;
    delete this.pids[which];
    if (this.state === "stopping" || this.state === "stopped") return;
    const why = signal ? `signal ${signal}` : `exit code ${code}`;
    if (which === "arm") {
      void this.killAll().then(() => {
        if (code === 0) this.setState("stopped", "The emulator powered off");
        else this.setState("crashed", `QEMU stopped (${why})\n${tail("qemu.out")}`);
      });
    } else {
      this.message = `The DSP process stopped (${why}). ${tail("q6.out", 3)}`;
      this.emit("status");
    }
  }

  private async attach() {
    /* the web UI has its own QMP socket, so tools/*.py can still use qmp.sock */
    this.qmp = await Qmp.connect(fs.existsSync(SOCK.webqmp) ? SOCK.webqmp : SOCK.qmp);
    this.qmp.on("close", () => {
      this.qmp = null;
      if (this.external && this.state === "running") {
        this.external = false;
        this.cleanupClients();
        this.setState("stopped", "The emulator exited");
      }
    });
    if (this.running?.dsp === "lle") {
      /* a QEMU we just launched creates q6webqmp.sock a moment later; an adopted one may predate it */
      const q6 = this.hexagon || fs.existsSync(SOCK.q6webqmp) ? SOCK.q6webqmp : SOCK.q6qmp;
      if (await waitFor(q6, 6000)) this.q6qmp = await Qmp.connect(q6).catch(() => null);
    }
    await this.connectVnc();
    void this.refreshSensors();
  }

  private async connectVnc() {
    const cfg = this.running;
    const dsp = cfg?.baseband === "lle" && cfg?.dsp === "lle";
    const toBrowser = !cfg || cfg.audio === "browser" || cfg.audio === "both";
    if (!(await waitFor(SOCK.vnc, 4000))) return;
    try {
      const v = await VncClient.connect(SOCK.vnc, { video: true, audio: toBrowser && !dsp });
      this.vnc = v;
      v.on("frame", (fb: Buffer, w: number, h: number) => this.hub.onFrame(fb, w, h));
      v.on("audio", (pcm: Buffer) => this.hub.onAudio(pcm));
      v.on("close", () => {
        if (this.vnc === v) this.vnc = null;
      });
    } catch (e) {
      this.message = `No picture: ${(e as Error).message}`;
    }
    if (dsp && toBrowser && (await waitFor(SOCK.q6vnc, 4000))) {
      try {
        const a = await VncClient.connect(SOCK.q6vnc, { video: false, audio: true });
        this.q6vnc = a;
        a.on("audio", (pcm: Buffer) => this.hub.onAudio(pcm));
        a.on("close", () => {
          if (this.q6vnc === a) this.q6vnc = null;
        });
      } catch {
        /* the DSP's audio stays on the host */
      }
    }
  }

  private async bootKeys(cfg: EmulatorConfig) {
    await sleep(2800);
    const keys = cfg.boot === "recovery" ? ["pgdn", "pgdn", "f2"] : ["f2"];
    for (const k of keys) {
      if (!this.qmp) return;
      this.sendQcode(k, true);
      await sleep(200);
      this.sendQcode(k, false);
      await sleep(300);
    }
  }

  private cleanupClients() {
    this.vnc?.close();
    this.q6vnc?.close();
    this.qmp?.close();
    this.q6qmp?.close();
    this.vnc = this.q6vnc = null;
    this.qmp = this.q6qmp = null;
    this.hub.reset();
  }

  private async killAll() {
    const procs = [this.arm, this.hexagon].filter((p): p is ChildProcess => p !== null);
    const quits = [this.qmp, this.q6qmp].filter((q): q is Qmp => q !== null).map((q) => q.execute("quit").catch(() => {}));
    await Promise.race([Promise.all(quits), sleep(1500)]);
    const deadline = Date.now() + 4000;
    while (procs.some((p) => p.exitCode === null && p.signalCode === null) && Date.now() < deadline) await sleep(100);
    for (const p of procs) if (p.exitCode === null && p.signalCode === null) p.kill("SIGKILL");
    if (this.external) {
      const end = Date.now() + 4000;
      while ((await canConnect(SOCK.qmp)) && Date.now() < end) await sleep(150);
    }
    this.cleanupClients();
    this.arm = this.hexagon = null;
    this.pids = {};
    this.external = false;
    this.running = null;
    fs.rmSync(RUN_FILE, { force: true });
  }

  private sendQcode(qcode: string, down: boolean) {
    this.qmp?.send("input-send-event", { events: [{ type: "key", data: { down, key: { type: "qcode", data: qcode } } }] });
  }

  input(m: InputMessage) {
    if (!this.qmp) return;
    if (m.t === "key") {
      const q = HARD_KEY_QCODE[m.k as HardKey];
      if (q) this.sendQcode(q, m.d);
      return;
    }
    const x = Math.round(Math.min(1, Math.max(0, m.x)) * 0x7fff);
    const y = Math.round(Math.min(1, Math.max(0, m.y)) * 0x7fff);
    const events: object[] = [];
    if (x !== this.touch.x || y !== this.touch.y || m.a === "down") {
      events.push({ type: "abs", data: { axis: "x", value: x } }, { type: "abs", data: { axis: "y", value: y } });
      this.touch.x = x;
      this.touch.y = y;
    }
    if (m.a === "down" && !this.touch.down) events.push({ type: "btn", data: { down: true, button: "left" } });
    if (m.a === "up" && this.touch.down) events.push({ type: "btn", data: { down: false, button: "left" } });
    this.touch.down = m.a === "down" ? true : m.a === "up" ? false : this.touch.down;
    if (events.length) this.qmp.send("input-send-event", { events });
  }

  async refreshSensors() {
    if (!this.qmp) return this.sensors;
    try {
      const cable = (await this.qmp.execute("qom-get", { path: "/machine", property: "cable" })) as Cable;
      const battery = (await this.qmp.execute("qom-get", { path: "/machine", property: "battery-capacity" })) as number;
      this.sensors = { cable, battery: Number(battery) };
    } catch {
      /* older builds lack the properties */
    }
    return this.sensors;
  }

  async setCable(cable: Cable) {
    if (!this.qmp) throw new Error("The emulator is not running");
    await this.qmp.execute("qom-set", { path: "/machine", property: "cable", value: cable });
    await this.refreshSensors();
    this.emit("status");
  }

  async setBattery(level: number) {
    if (!this.qmp) throw new Error("The emulator is not running");
    await this.qmp.execute("qom-set", { path: "/machine", property: "battery-capacity", value: Math.round(level) });
    await this.refreshSensors();
    this.emit("status");
  }

  async setGps(lat: number, lon: number, alt: number) {
    if (!this.qmp) throw new Error("The emulator is not running");
    if (this.running?.baseband === "lle") throw new Error("GPS fixes come from the modelled baseband (HLE mode)");
    await this.qmp.execute("qom-set", { path: "/machine/modem", property: "gps-fix", value: `${lat},${lon},${alt}` });
  }

  get screen() {
    return this.hub.lastFrame ?? null;
  }

  static readonly SCREEN = { w: SCREEN_W, h: SCREEN_H };
}
