import net from "node:net";
import { EventEmitter } from "node:events";

type Pending = { resolve: (v: unknown) => void; reject: (e: Error) => void };

export class Qmp extends EventEmitter {
  private sock: net.Socket | null = null;
  private buf = "";
  private pending: Pending[] = [];
  private ready = false;

  static connect(path: string, timeoutMs = 3000): Promise<Qmp> {
    return new Promise((resolve, reject) => {
      const q = new Qmp();
      const sock = net.createConnection(path);
      const timer = setTimeout(() => {
        sock.destroy();
        reject(new Error("QMP handshake timed out"));
      }, timeoutMs);
      sock.setNoDelay(true);
      sock.on("error", (e) => {
        clearTimeout(timer);
        if (!q.ready) reject(e);
        q.close();
      });
      sock.on("close", () => q.close());
      sock.on("data", (d) => q.onData(d.toString("utf8")));
      q.sock = sock;
      q.once("greeting", () => {
        q.execute("qmp_capabilities")
          .then(() => {
            clearTimeout(timer);
            q.ready = true;
            resolve(q);
          })
          .catch(reject);
      });
    });
  }

  get connected() {
    return this.ready && this.sock !== null;
  }

  private onData(s: string) {
    this.buf += s;
    let nl;
    while ((nl = this.buf.indexOf("\n")) >= 0) {
      const line = this.buf.slice(0, nl).trim();
      this.buf = this.buf.slice(nl + 1);
      if (!line) continue;
      let m: Record<string, unknown>;
      try {
        m = JSON.parse(line);
      } catch {
        continue;
      }
      if ("QMP" in m) this.emit("greeting", m);
      else if ("event" in m) this.emit("event", m);
      else if ("return" in m || "error" in m) {
        const p = this.pending.shift();
        if (!p) continue;
        if ("error" in m) {
          const e = m.error as { desc?: string };
          p.reject(new Error(e.desc ?? "QMP error"));
        } else p.resolve(m.return);
      }
    }
  }

  execute(cmd: string, args?: Record<string, unknown>): Promise<unknown> {
    const sock = this.sock;
    if (!sock) return Promise.reject(new Error("QMP not connected"));
    return new Promise((resolve, reject) => {
      this.pending.push({ resolve, reject });
      sock.write(JSON.stringify(args ? { execute: cmd, arguments: args } : { execute: cmd }) + "\n");
    });
  }

  /** Fire-and-forget: input events are sent in order and their replies ignored. */
  send(cmd: string, args?: Record<string, unknown>) {
    this.execute(cmd, args).catch(() => {});
  }

  hmp(command: string) {
    return this.execute("human-monitor-command", { "command-line": command }) as Promise<string>;
  }

  close() {
    if (!this.sock) return;
    const s = this.sock;
    this.sock = null;
    this.ready = false;
    s.destroy();
    for (const p of this.pending.splice(0)) p.reject(new Error("QMP closed"));
    this.emit("close");
  }
}
