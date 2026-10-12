#!/bin/sh
# magkey.sh KEY[:ms]... - press keys one at a time (1 s apart) and save a
# small screenshot to build-tmp/key.png; for driving MAGLDR's menus.
cd "$(dirname "$0")/../images" || exit 1
for k in "$@"; do python3 ../tools/keys.py qmp.sock "$k"; sleep 1; done
python3 ../tools/qmon.py qmp.sock "screendump $PWD/../build-tmp/key.ppm"
python3 -c "from PIL import Image; Image.open('../build-tmp/key.ppm').crop((0,0,480,480)).resize((240,240)).save('../build-tmp/key.png')"
