#!/bin/sh
# Build everything this project ships - the QEMU machine, the web UI and the
# NAND images - and, with the GitHub CLI, put it in a release.
# usage: tools/release.sh [options]
#   --rom PATH|URL       NAND ROM zip       (default $ROM_URL)
#   --clk PATH|URL       cCLK: LEOIMG.nbh or lk.bin (default $CLK_URL)
#   --no-download        use only the sources already in build/sources
#   --radio PATH|URL     radio RUU_signed.nbh or the RUU zip (default $RADIO_URL)
#   --recovery PATH|URL  image for the recovery partition (default $RECOVERY_URL)
#   --out DIR            where to assemble   (default build/release)
#   --tag TAG            release tag         (default $TAG, else the git ref)
#   --upload             gh release create/upload the assets
#   --skip-qemu          use src/qemu/build as it is, do not build it
#   --skip-web           do not build the web UI
#   --skip-nand          do not build the NAND images (repack the bundle only)
#   --offline            copy web/node_modules instead of `npm ci`
#   --keep               keep build/ (default: only the bundles are kept)
#   --list               print what the sources resolve to and stop
#
# Sources are paths or URLs; a mega.nz link needs megatools (`megaget`) and a
# plain https one just curl.  With none of clk/rom given, the NAND images are
# skipped and the rest is still built - that is what you want in CI when the
# ROM lives behind a link you have not turned into a variable yet.
set -e
cd "$(dirname "$0")/.." || exit 1
ROOT=$(pwd)
BUILD=$ROOT/build                 # this script's scratch
QEMU_BUILD=$ROOT/src/qemu/build   # where QEMU itself is built
OUT=$BUILD/release
SRC=$BUILD/sources
WORK=$BUILD/work
ROMAIN=https://github.com/HTC-Leo-Revival-Project/HTC-HD7-Internals/raw/refs/heads/main/roms
ROM=${ROM_URL:-$ROMAIN/NexusHD2-ICS-CM9-HWA_V3.0b.zip}
CLK=${CLK_URL:-$ROMAIN/cLK.nbh}
RADIO=${RADIO_URL:-$ROMAIN/Leo/1.%20Radio_Leo_2.15.50.14.zip}
RECOVERY=${RECOVERY_URL:-}
TAG=${TAG:-}; UPLOAD=${UPLOAD:-0}; SKIP_QEMU=; SKIP_WEB=; SKIP_NAND=; OFFLINE=; NO_DOWNLOAD=; KEEP=; LIST=0
SYSTEM_MB=${SYSTEM_MB:-220}; CACHE_MB=${CACHE_MB:-30}

while [ $# -gt 0 ]; do
  case $1 in
    --rom) ROM=$2; shift 2 ;;
    --clk) CLK=$2; shift 2 ;;
    --radio) RADIO=$2; shift 2 ;;
    --recovery) RECOVERY=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    --tag) TAG=$2; shift 2 ;;
    --upload) UPLOAD=1; shift ;;
    --skip-qemu) SKIP_QEMU=1; shift ;;
    --skip-web) SKIP_WEB=1; shift ;;
    --skip-nand) SKIP_NAND=1; shift ;;
    --offline) OFFLINE=1; shift ;;
    --no-download) NO_DOWNLOAD=1; shift ;;
    --keep) KEEP=1; shift ;;
    --list) LIST=1; shift ;;
    -h|--help) sed -n '2,21p' "$0" | cut -c3-; exit 0 ;;
    *) echo "unknown option $1 (try --help)"; exit 1 ;;
  esac
done
[ -n "$TAG" ] || TAG=$(git rev-parse --short HEAD 2>/dev/null || echo local)
# UPLOAD may come from the environment (CI sets it to "" or 1) or from --upload
case ${UPLOAD:-} in ''|0|false|no) UPLOAD=0 ;; *) UPLOAD=1 ;; esac

die() { echo "error: $*" >&2; exit 1; }
step() { echo; echo "== $*"; }
have() { command -v "$1" >/dev/null 2>&1; }

# ------------------------------------------------------------- sources
# get NAME SOURCE: a local path is copied, an https one is curled, a mega.nz
# one is fetched with megatools.  Prints the path of the file.
get() {
  _name=$1; _src=$2
  [ -n "$_src" ] || return 1
  if [ -f "$_src" ]; then echo "$_src"; return 0; fi
  mkdir -p "$SRC"
  # --no-download: reuse whatever a previous run already fetched
  if [ -n "$NO_DOWNLOAD" ]; then
    case $_src in
      *NexusHD2*) _cached=$SRC/NexusHD2-ICS-CM9-HWA_V3.0b.zip ;;
      *cLK*) _cached=$SRC/cLK.nbh ;;
      *Radio*) _cached=$(find "$SRC" -iname '*Radio*.nbh' -print -quit 2>/dev/null) ;;
      *) _cached= ;;
    esac
    [ -n "$_cached" ] && [ -f "$_cached" ] || return 1
    echo "$_cached"; return 0
  fi
  _out=""
  case $_src in
    *mega.nz*)
      have megaget || die "mega.nz source '$_src' needs megatools: apt install megatools, or pass a local copy"
      echo "  megaget $_src" >&2
      : > "$SRC/.stamp"
      ( cd "$SRC" && megaget --download --path "$_src" ) >&2 ||
        die "megaget failed on $_src"
      # megaget names files after the folder, and a folder link can bring a
      # tree: take the largest thing it just wrote
      _out=$(find "$SRC" -newer "$SRC/.stamp" -type f -printf '%s %p\n' 2>/dev/null |
             sort -rn | head -1 | cut -d' ' -f2-)
      rm -f "$SRC/.stamp"
      [ -n "$_out" ] || die "megaget downloaded nothing from $_src"
      ;;
    http://*|https://*)
      # keep the name from the URL, so a .zip stays recognisable as one
      _out=$SRC/$(basename "${_src%%\?*}")
      echo "  curl $_src" >&2
      curl -fsSL --retry 3 -o "$_out" "$_src" || die "download failed: $_src"
      ;;
    *) die "no such file, and not a URL: $_src" ;;
  esac
  [ -f "$_out" ] || die "nothing downloaded for $_src"
  echo "$_out"
}

# the radio is distributed as a zip inside a zip with the NBH inside that
find_ruu() {
  _f=$1
  case $(head -c 2 "$_f") in
    PK) : ;;
    *) echo "$_f"; return 0 ;;
  esac
  _dir=$SRC/ruu
  rm -rf "$_dir"; mkdir -p "$_dir"
  unzip -o -q "$_f" -d "$_dir" || die "cannot unpack $_f"
  # one or more levels deep, and the name is not always RUU_signed.nbh
  _n=0
  while [ -z "$(find "$_dir" -iname '*.nbh' -print -quit)" ] && [ $_n -lt 4 ]; do
    # read it quoted: these names have spaces in them
    find "$_dir" -iname '*.zip' -print | while IFS= read -r _z; do
      _sub=$_dir/x${_n}_$(basename "$_z"); mkdir -p "$_sub"
      unzip -o -q "$_z" -d "$_sub" 2>/dev/null || :
    done
    _n=$((_n + 1))
  done
  _nbh=$(find "$_dir" -iname '*.nbh' | head -1)
  [ -n "$_nbh" ] || die "no .nbh inside $_f (found: $(find "$_dir" -type f | wc -l) files)"
  cp "$_nbh" "$SRC/RUU_signed.nbh"
  echo "$SRC/RUU_signed.nbh"
}

step "sources"
CLK_FILE=$(get clk "$CLK") || CLK_FILE=
ROM_FILE=$(get rom "$ROM") || ROM_FILE=
RADIO_FILE=$(find_ruu "$(get radio "$RADIO")") || die "no --radio: nand-lle.img needs it"
REC_FILE=$(get recovery "$RECOVERY") || REC_FILE=
echo "  clk       ${CLK_FILE:-(none)}"
echo "  rom       ${ROM_FILE:-(none)}"
echo "  radio     $RADIO_FILE"
echo "  recovery  ${REC_FILE:-(none)}"
echo "  tag       $TAG"

# check the ROM before spending an hour on it: hd2_mknand.py needs a NAND ROM
if [ -n "$ROM_FILE" ]; then
  case $ROM_FILE in
    *.zip)
      missing=
      for f in kernel/zImage kernel/boot_NAND/initrd.gz kernel/mkbootimg.sh \
               META-INF/com/google/android/updater-script; do
        unzip -Z1 "$ROM_FILE" | grep -qx "$f" || missing="$missing $f"
      done
      [ -z "$missing" ] || die "$ROM_FILE is not a NAND ROM (missing:$missing). Needs kernel/zImage, kernel/boot_NAND/initrd.gz, kernel/mkbootimg.sh and the updater-script; an SD-only zip will not work."
      ;;
    *) die "the ROM has to be the zip" ;;
  esac
fi
# Which of cLK's two partition schemes is this?  An NBH wraps either a 1.4 LK,
# whose partitions are a table compiled into the LK (PTABLE-MB) and start at
# block 0x219, or a 1.5 one, which reads a VPTABLE from the image instead.
# hd2_mknand.py only lays the 1.4 image out correctly when it is given the raw
# LK (that is the path that reads and resizes PTABLE-MB); --nbh always takes the
# 1.5 branch.  So look inside, rather than trusting the R000FF magic.
CLK_ARG=; CLK_KIND=
if [ -n "$CLK_FILE" ]; then
  if [ "$(head -c 6 "$CLK_FILE")" = R000FF ]; then
    RAW_LK=$SRC/$(basename "$CLK_FILE" .nbh).lk.bin
    python3 - "$ROOT/tools/hd2_mknand.py" "$CLK_FILE" "$RAW_LK" <<'PY' ||
import sys, importlib.util
spec = importlib.util.spec_from_file_location("mknand", sys.argv[1])
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
secs, _start, count = m.extract_nbh_os(sys.argv[2])
# the LK proper is the OS section; keep it whole, the trailing padding is inert
open(sys.argv[3], "wb").write(b"".join(secs[2:2 + count]))
PY
    die "cannot read the LK out of $CLK_FILE"
    if grep -qa 'PTABLE-MB' "$RAW_LK"; then
      CLK_ARG="--lk $RAW_LK"; CLK_KIND="cLK 1.4 (compiled table, extracted from the NBH)"
    else
      CLK_ARG="--nbh $CLK_FILE"; CLK_KIND="cLK 1.5 (VPTABLE)"
    fi
  else
    CLK_ARG="--lk $CLK_FILE"; CLK_KIND="raw LK"
  fi
  echo "  clk is    $CLK_KIND"
fi
if [ "$LIST" = 1 ]; then echo; echo "--list: nothing built"; exit 0; fi

rm -rf "$OUT"; mkdir -p "$OUT"
rm -rf "$WORK"; mkdir -p "$WORK"

# ------------------------------------------------------------- qemu
if [ -z "$SKIP_QEMU" ]; then
  step "qemu"
  mkdir -p "$QEMU_BUILD"
  if [ ! -f "$QEMU_BUILD/build.ninja" ]; then
    ( cd "$QEMU_BUILD" && "$ROOT/src/qemu/configure" --target-list=arm-softmmu,hexagon-softmmu \
        --enable-sdl --disable-docs --disable-werror --disable-gtk --disable-xen \
        --disable-kvm --disable-tools --disable-guest-agent ) ||
      die "configure failed"
  fi
  # VNC for the web UI, the plugins for AMSSLOG
  ( cd "$QEMU_BUILD" && ./pyvenv/bin/meson configure . -Dvnc=enabled >/dev/null )
  ( cd "$QEMU_BUILD" && ninja qemu-system-arm qemu-system-hexagon \
      contrib/plugins/libamsslog.so contrib/plugins/libhexhook.so ) ||
    die "ninja failed: the build needs libglib2.0-dev, libpixman-1-dev, libslirp-dev, libsdl2-dev"
else
  step "qemu (skipped, using src/qemu/build as it is)"
fi
[ -x "$QEMU_BUILD/qemu-system-arm" ] || die "no $QEMU_BUILD/qemu-system-arm: build it, or drop --skip-qemu"

# ------------------------------------------------------------- web
if [ -z "$SKIP_WEB" ]; then
  step "web ui"
  command -v node >/dev/null || die "node not found: the server runs server/index.ts, so it needs node >= 22.18"
  [ "$(printf '22.18\n%s\n' "$(node -p 'process.versions.node')" | sort -V | head -1)" = 22.18 ] ||
    die "node $(node -p 'process.versions.node') is too old; needs >= 22.18"
  ( cd "$ROOT/web" &&
    if [ -n "$OFFLINE" ]; then echo "  npm: offline copy"; :
    else npm ci; fi &&
    npm run build ) || die "web build failed"
else
  step "web ui (skipped)"
fi

# ------------------------------------------------------------- nand
if [ -n "$SKIP_NAND" ]; then
  step "nand images (skipped)"
elif [ -n "$CLK_FILE" ] && [ -n "$ROM_FILE" ]; then
  step "nand images"
  [ -n "$REC_FILE" ] && REC="--recovery $REC_FILE"
  python3 "$ROOT/tools/hd2_mknand.py" $CLK_ARG --rom "$ROM_FILE" $REC \
      --system-mb "$SYSTEM_MB" --cache-mb "$CACHE_MB" -o "$WORK/nand.img" ||
    die "hd2_mknand.py failed"
  cp "$WORK/nand.img" "$WORK/nand-lle.img"
  python3 "$ROOT/tools/hd2_radio.py" "$RADIO_FILE" -o "$WORK/amss.elf" \
      --dsp "$WORK/dsp1.elf" --write-nand "$WORK/nand-lle.img" >/dev/null ||
    die "hd2_radio.py failed"
  # a couple of NAND images do not fit in a release one by one, well, they do,
  # but they are mostly erased flash: zstd takes a long while to nothing
  step "packing the nand images"
  for f in nand.img nand-lle.img; do
    zstd -q -T0 -3 "$WORK/$f" -o "$OUT/$f.zst" || die "zstd failed"
    echo "  $f.zst $(du -h "$OUT/$f.zst" | cut -f1)"
  done
else
  echo; echo "warning: no clk and/or rom: skipping the nand images"
fi

# ------------------------------------------------------------- bundles
step "bundles"
# the tree the web server wants, minus the images (they are separate assets)
BUNDLE=$WORK/web-bundle
mkdir -p "$BUNDLE/web" "$BUNDLE/src/qemu/build/contrib/plugins" "$BUNDLE/tools"
rsync -a --exclude node_modules --exclude .next "$ROOT/web/" "$BUNDLE/web/"
[ -d "$ROOT/web/.next" ] && cp -a "$ROOT/web/.next" "$BUNDLE/web/.next"
rsync -a --exclude node_modules "$ROOT/tools/" "$BUNDLE/tools/"
cp -p "$QEMU_BUILD/qemu-system-arm" "$QEMU_BUILD/qemu-system-hexagon" "$BUNDLE/src/qemu/build/"
cp -a "$ROOT/src/qemu/pc-bios" "$BUNDLE/src/qemu/" 2>/dev/null || true
cp -a "$QEMU_BUILD/pc-bios" "$BUNDLE/src/qemu/build/" 2>/dev/null || true
cp -a "$QEMU_BUILD/contrib/plugins/"*.so "$BUNDLE/src/qemu/build/contrib/plugins/" 2>/dev/null || true

Z=zstd
tar -C "$(dirname "$BUNDLE")" -cf - web-bundle | \
    $Z -q -T0 -3 -o "$OUT/htcleo-web-$TAG.tar.zst" -f
echo "  htcleo-web-$TAG.tar.zst $(du -h "$OUT/htcleo-web-$TAG.tar.zst" | cut -f1)"

( cd "$OUT" && sha256sum ./* > SHA256SUMS )
for f in "$OUT"/*; do printf '  %8s  %s\n' "$(du -h "$f" | cut -f1)" "$(basename "$f")"; done

# ------------------------------------------------------------- release
if [ "$UPLOAD" = 1 ]; then
  step "gh release"
  command -v gh >/dev/null || die "gh not found: https://cli.github.com"
  gh auth status >&2 || die "gh is not logged in (GH_TOKEN?)"
  if gh release view "$TAG" >/dev/null 2>&1; then
    echo "  updating $TAG"
  else
    gh release create "$TAG" --title "$TAG" --generate-notes \
        --notes "Built by tools/release.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ)."
  fi
  gh release upload "$TAG" "$OUT"/* --clobber
  echo "  https://github.com/$(gh repo view --json nameWithOwner -q .nameWithOwner)/releases/tag/$TAG"
fi

[ -n "$KEEP" ] || rm -rf "$WORK"
echo; echo "done. assets in $OUT"