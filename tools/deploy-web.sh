#!/bin/sh
# Install the web UI on this machine so the emulator can be reached from a
# browser anywhere: copy what it needs out of the repo, build it, and (with
# sudo) add a systemd service and an nginx site with TLS.
# usage: [sudo] tools/deploy-web.sh [options]
#   --dest DIR     where to install (default /opt/htcleo-web)
#   --user USER    service user (default: the calling user)
#   --port N       port (default 3000)
#   --host ADDR    what it binds (default 127.0.0.1; put nginx in front)
#   --domain NAME  add an nginx site for NAME and get a certificate with certbot
#   --img GLOB     only copy images matching GLOB (repeatable, default: all)
#   --no-images    install the app but no images/*.img (the UI then has none)
#   --skip-copy    DEST is already an unpacked tree (a release bundle): only
#                  build and install, do not copy anything
#   --no-service   do not touch systemd
#   --no-nginx     do not touch nginx
#   --no-start     install but do not start
#   --offline      copy node_modules instead of running `npm ci` (same machine,
#                  same CPU; skips the network)
#
# The server runs QEMU itself, so it needs the two binaries, the NAND images and
# pc-bios (VNC's keymaps) - it is not a static site and cannot be deployed to a
# host that only serves files.  It has no login: whoever reaches it can drive the
# emulator and type arbitrary extra QEMU arguments into it, so keep it on
# loopback behind nginx and TLS, as this does by default.
set -e
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)

DEST=/opt/htcleo-web
USER=$(id -un)
PORT=3000
HOST=127.0.0.1
DOMAIN=
SERVICE=1; NGINX=1; START=1; OFFLINE=; IMAGES=1; IMG_GLOBS=; SKIP_COPY=0
while [ $# -gt 0 ]; do
  case $1 in
    --dest) DEST=$2; shift 2 ;;
    --user) USER=$2; shift 2 ;;
    --port) PORT=$2; shift 2 ;;
    --host) HOST=$2; shift 2 ;;
    --domain) DOMAIN=$2; NGINX=1; shift 2 ;;
    --img) IMG_GLOBS="$IMG_GLOBS $2"; shift 2 ;;
    --no-images) IMAGES=0; shift ;;
    --skip-copy) SKIP_COPY=1; shift ;;
    --no-service) SERVICE=0; shift ;;
    --no-nginx) NGINX=0; shift ;;
    --no-start) START=0; shift ;;
    --offline) OFFLINE=1; shift ;;
    -h|--help) sed -n '2,24p' "$0" | cut -c3-; exit 0 ;;
    *) echo "unknown option $1 (try --help)"; exit 1 ;;
  esac
done
SVC=htcleo-web

die() { echo "error: $*" >&2; exit 1; }
step() { echo; echo "== $*"; }

# ---------------------------------------------------------------- preflight
step "checking what is here"
command -v node >/dev/null || die "node not found: the server runs server/index.ts straight, so it needs node >= 22.18 (typescript without a build step)"
NV=$(node -p 'process.versions.node' 2>/dev/null || echo 0)
node_ok=$(printf '22.18\n%s\n' "$NV" | sort -V | head -1)
[ "$node_ok" = 22.18 ] || die "node $NV is too old; needs >= 22.18 for node's own typescript stripping"
[ -x src/qemu/build/qemu-system-arm ] || die "src/qemu/build/qemu-system-arm not found: build it first (README, Build)"
[ -x src/qemu/build/qemu-system-hexagon ] || echo "warning: no qemu-system-hexagon: the QDSP6 firmware will not run"
# VNC is what the web UI reads the screen and the sound through.  The option
# exists even when the feature does not, so look for the code itself.
for b in qemu-system-arm qemu-system-hexagon; do
  [ -x "src/qemu/build/$b" ] || continue
  strings "src/qemu/build/$b" | grep -q vnc_set_share_mode ||
    die "$b has no VNC: every build here has it, so this one is stale; rebuild with tools/build-linux.sh (docs/building.md)"
done
[ -d src/qemu/pc-bios/keymaps ] || die "src/qemu/pc-bios/keymaps is missing: VNC needs its keymaps from there (-L)"
command -v ffmpeg >/dev/null || die "ffmpeg not found: the server pipes the screen through it (libx264, or h264_nvenc)"
ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libx264 ||
  echo "warning: this ffmpeg has no libx264; the UI falls back to JPEG pictures"
ls images/*.img >/dev/null 2>&1 ||
  echo "warning: no images/*.img: the UI can start the emulator only after you put a NAND image there"
echo "node $NV, ffmpeg $(ffmpeg -hide_banner -version 2>/dev/null | sed -n '1s/.*version \([0-9.]*\).*/\1/p'), images in $ROOT/images"

# ---------------------------------------------------------------- copy
if [ "$SKIP_COPY" = 1 ]; then
  step "installing from $ROOT into $DEST (nothing to copy, --skip-copy)"
  [ -d "$ROOT/web" ] || die "--skip-copy, but there is no web/ under $ROOT"
  [ -d "$DEST/images" ] || mkdir -p "$DEST/images"
else
  step "installing into $DEST (as $USER)"
  command -v rsync >/dev/null || die "rsync not found: install it, this script uses it to copy"
  [ -w / ] || [ -w "$(dirname "$DEST")" ] || die "cannot write $DEST: run this with sudo, or pick another --dest"
  mkdir -p "$DEST/src/qemu/build/contrib/plugins"
  [ "$IMAGES" = 1 ] && mkdir -p "$DEST/images"
  # the UI itself
  rsync -a --delete --exclude node_modules --exclude .next --exclude .git \
        "$ROOT/web/" "$DEST/web/"
  # what it spawns and reads
  for b in qemu-system-arm qemu-system-hexagon; do
    [ -f "src/qemu/build/$b" ] && cp -p "src/qemu/build/$b" "$DEST/src/qemu/build/$b"
  done
  cp -a src/qemu/build/pc-bios "$DEST/src/qemu/build/" 2>/dev/null || true
  cp -a src/qemu/pc-bios "$DEST/src/qemu/" 2>/dev/null || true
  cp -a src/qemu/build/contrib/plugins/*.so "$DEST/src/qemu/build/contrib/plugins/" 2>/dev/null || true
fi
# the images the UI lists (images/*.img), and the firmware it checks for.  Its
# own scratch (sockets, logs, dumps) stays behind.
if [ "$IMAGES" = 1 ]; then
  # pick the images first, so we can check for room before copying gigabytes.
  # The list holds names relative to images/, which is what rsync wants.
  LIST=$(mktemp)
  if [ -n "$IMG_GLOBS" ]; then
    ( cd "$ROOT/images" && ls -1 $IMG_GLOBS ) > "$LIST" 2>/dev/null || :
  else
    ( cd "$ROOT/images" && ls -1 *.img ) > "$LIST" 2>/dev/null || :
  fi
  NEED=$( ( cd "$ROOT/images" && du -cb $(cat "$LIST") ) 2>/dev/null | tail -1 | cut -f1)
  [ -n "$NEED" ] || NEED=0
  AVAIL=$(df -Pk "$DEST" | awk 'NR==2 {print $4 * 1024}')
  if [ "$NEED" -gt 0 ] && [ $((AVAIL - NEED)) -lt 1073741824 ]; then
    rm -f "$LIST"
    die "not enough room in $DEST: $((NEED / 1048576)) MiB of images, $((AVAIL / 1048576)) MiB free (1 GiB kept free). Use --img to copy fewer, or another --dest"
  fi
  echo "copying images ($((NEED / 1048576)) MiB):"
  sed 's/^/  /' "$LIST"
  [ "$NEED" = 0 ] || rsync -a --files-from="$LIST" "$ROOT/images/" "$DEST/images/"
  rm -f "$LIST"
fi
ls -la "$DEST/images/" 2>/dev/null | sed -n '2,$p' | awk '{printf "  %10s  %s\n", $5, $NF}'

# ---------------------------------------------------------------- build
step "building the web UI"
cd "$DEST/web"
# the release bundle (the hd2-web-ui artifact) is already built and carries no
# sources, so there is nothing to install or compile.
if [ -d .next ] && [ ! -d app ]; then
  echo "this tree is already built (.next but no sources): skipping npm ci and next build"
else
  if [ -n "$OFFLINE" ] && [ -d "$ROOT/web/node_modules" ]; then
    echo "copying node_modules from $ROOT/web (--offline)"
    cp -a "$ROOT/web/node_modules" .
  elif [ -d node_modules ] && [ -f node_modules/.package-lock.json ]; then
    echo "node_modules is there, skipping npm ci"
  else
    npm ci
  fi
  [ -f next.config.ts ] || die "next.config.ts missing from the copy"
  npm run build
fi
[ -d .next ] || die "no .next in $DEST/web: build it first (npm ci && npm run build), or use the hd2-web-ui release bundle"

# ---------------------------------------------------------------- service
if [ "$SERVICE" = 1 ]; then
  step "systemd service $SVC.service"
  [ "$(id -u)" = 0 ] || die "writing $SVC.service needs root: re-run with sudo, or --no-service"
  command -v systemctl >/dev/null || die "no systemd here: start it by hand ($DEST/web, NODE_ENV=production npm start)"
  UID_=$(id -u "$USER")
  cat > "/etc/systemd/system/$SVC.service" <<EOF
[Unit]
Description=HTC HD2 emulator web UI
After=network.target

[Service]
Type=simple
User=$USER
WorkingDirectory=$DEST/web
Environment=NODE_ENV=production HOST=$HOST PORT=$PORT
ExecStart=/usr/bin/env node server/index.ts
Restart=on-failure
RestartSec=2
# QEMU is spawned in its own process group and outlives a restart of the web
# server on purpose (the next one attaches to it again), so do not kill the
# group; kill the leftovers by hand if you really want them gone.
KillMode=process
TimeoutStopSec=10
NoNewPrivileges=yes
# images/ and .emulator.json are written at runtime
ProtectHome=read-only
ProtectSystem=full
ReadWritePaths=$DEST/images $DEST/web
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF
  systemctl daemon-reload
  systemctl enable "$SVC.service" >/dev/null
  echo "wrote /etc/systemd/system/$SVC.service (user $USER, $HOST:$PORT)"
fi

# ---------------------------------------------------------------- nginx
if [ "$NGINX" = 1 ]; then
  step "nginx"
  if [ -z "$DOMAIN" ]; then
    echo "skipped: pass --domain NAME for an https site (or --no-nginx)"
  else
    [ "$(id -u)" = 0 ] || die "writing the nginx site needs root: re-run with sudo, or --no-nginx"
    command -v nginx >/dev/null || die "nginx not found"
    [ -f /etc/nginx/sites-available/"$SVC" ] || cat > "/etc/nginx/sites-available/$SVC" <<EOF
# The emulator web UI.  Plain HTTP, so the browser has no secure context and
# falls back to JPEG instead of WebCodecs H.264; certbot turns this into https.
server {
    listen 80;
    listen [::]:80;
    server_name $DOMAIN;
    client_max_body_size 4m;

    location / {
        proxy_pass http://$HOST:$PORT;
        proxy_http_version 1.1;
        proxy_set_header Host \$host;
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto \$scheme;
        # /ws/video, /ws/audio and /ws/control are websockets
        proxy_set_header Upgrade \$http_upgrade;
        proxy_set_header Connection "upgrade";
        # video is a stream; do not buffer it
        proxy_buffering off;
        proxy_read_timeout 3600s;
    }
}
EOF
    ln -sf "/etc/nginx/sites-available/$SVC" "/etc/nginx/sites-enabled/$SVC"
    if command -v certbot >/dev/null; then
      echo "asking certbot for a certificate..."
      certbot --nginx -d "$DOMAIN" || echo "certbot failed: the site is up on http://$DOMAIN"
    else
      echo "certbot not found: install it, or the site stays http (no WebCodecs)"
    fi
    nginx -t && systemctl reload nginx
    echo "https://$DOMAIN"
  fi
fi

# ---------------------------------------------------------------- go
if [ "$START" = 1 ] && [ "$SERVICE" = 1 ]; then
  step "starting"
  systemctl restart "$SVC.service"
  sleep 2
  if curl -fsS -o /dev/null "http://$HOST:$PORT/"; then
    echo "serving on http://$HOST:$PORT (journalctl -u $SVC -f)"
  else
    echo "no answer on http://$HOST:$PORT yet; journalctl -u $SVC -n 50" >&2
  fi
fi

cat <<EOF

$(basename "$0") finished.

  installed in   $DEST
  service        $SVC.service (systemctl status / journalctl -u $SVC -f)
  settings       $DEST/web/.emulator.json  (the UI writes this)
  NAND images    $DEST/images/*.img
  logs           $DEST/images/qemu.out, q6.out, uart1.log, ramcon.log

The UI has no login and can start QEMU with whatever "extra args" you type into
it, so keep it behind TLS and do not bind it to a public address.  Copy new NAND
images to $DEST/images and restart the service to see them in the UI.
EOF