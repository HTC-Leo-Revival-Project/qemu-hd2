# HD2 Emulator web UI

A browser front end for the `-M htcleo` machine. It shows the live screen in a photo of a real HD2 and lets you use the hardware keys, touch and type. It also starts, stops and configures the emulator (HLE/LLE baseband, the QDSP6 firmware, images, GPU, audio).

```bash
cd web
npm install
npm run dev          # http://127.0.0.1:3000  (HOST=0.0.0.0 PORT=… to change)
npm run build && npm start   # production
```

QEMU is always built with VNC — the server reads the screen and the sound
through it ([docs/building.md](../docs/building.md)).

CI builds and packages the UI as the `hd2-web-ui` artifact: a `.tar.gz` you
can unpack and run with `NODE_ENV=production node server/index.ts`.

## How it works

```
qemu-system-arm ──VNC (unix socket: raw BGRX + QEMU audio ext.)──┐
qemu-system-hexagon ──VNC (audio only, qdsp6=lle)────────────────┤
                                                                 ▼
                       server/  (Node, the same process as Next.js)
                        ├─ vnc.ts      RFB client: frames, PCM
                        ├─ encoder.ts  ffmpeg: x264 or NVENC → FLV tags → H.264 access units
                        ├─ stream.ts   one encoder shared by every viewer; a new viewer gets a keyframe
                        ├─ qmp.ts      input-send-event (touch, keys), qom-set (battery, cable, GPS)
                        └─ emulator.ts launches QEMU the way tools/start.sh / start-q6.sh do
                                                                 │ WebSocket
                                                                 ▼
                       browser: WebCodecs VideoDecoder → <canvas>, AudioWorklet ← PCM
```

- **Video**: `/ws/video`. ffmpeg writes FLV because every FLV tag carries its own length, so each frame goes out as soon as it is encoded; Annex B would hold the newest frame back until the next start code. The FLV sequence header is the avcC record that WebCodecs takes as its `description`. Without WebCodecs (it needs a secure context, so plain http from another machine), the page asks for `?format=jpeg` and gets JPEG pictures instead.
- **Latency**: QEMU's VNC server polls the display at 60 Hz, and at 30 Hz when idle (`ui/vnc.c`). A tap reaches the screen in about 50 ms, most of it Android drawing the frame.
- **Audio**: `/ws/audio` carries 48 kHz stereo S16 PCM from QEMU's VNC audio capture; `public/pcm-player.js` keeps about 60 ms queued.
- **Input**: `/ws/control` takes `{t:"touch",a,x,y}` (0..1 screen coordinates) and `{t:"key",k,d}` (`volup voldown menu call end back home power`), and pushes the emulator status once a second. Typed text and Enter/Backspace/arrows go through `adb shell input`.
- **QMP**: the UI uses its own sockets (`images/webqmp.sock`, `q6webqmp.sock`), so `tools/keys.py`, `touch.py` and `qmon.py` keep working on `qmp.sock`.
- QEMU runs in its own process group: restarting the web server leaves it running, and the next server attaches to it again. An emulator started with `tools/start*.sh` is attached too, but without a picture, since it has no VNC socket.
- Settings are saved in `web/.emulator.json`.
- NAND images are listed from `images/`. A stock Windows Mobile 6.5 ROM (its WinCE MBR lists ULDR first) shows as "Windows Mobile 6.5": it always runs on the real baseband (LLE), with or without the DSP firmware, and boots straight from NAND (the machine loads the ROM's XIP partition, as the HTC SPL does), so the cLK boot choices do not apply.
- With the real DSP, the UI runs qemu-system-hexagon with `-icount shift=auto` by default, so the DSP's virtual clock follows real time. With a fixed shift, the DSP's audio DMA runs at about half speed while the DSP is busy. `Q6_ARGS=… npm run dev` adds arguments to the Hexagon process, as in `tools/start-q6.sh`.

## Keyboard (screen focused)

| Key | HD2 |
|---|---|
| Esc | Back |
| Home | Home |
| F1 / Menu | Windows (Menu) |
| F2 / F3 | Call / End |
| F12 | Power |
| PgUp / PgDn | Volume |
| letters, Enter, Backspace, arrows | typed through adb |

## Credits

The device frame is derived from [“HTC HD2 - Front”](https://commons.wikimedia.org/wiki/File:HTC_HD2_-_Front.jpg) by Beamish4, CC BY-SA 4.0: straightened to a front view, with the background and the screen cut out (`public/hd2-frame.LICENSE.txt`).
