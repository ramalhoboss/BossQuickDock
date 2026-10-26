#!/bin/sh
# Builds assets/pic0.png: the 1920x1080 background the console shows while the
# BossQuickDock tile is selected. The logo card (assets/brand/logo-src.jpg,
# corners rounded off) sits on the right over a soft blue glow, which leaves
# the left side clear for the home screen's own text. Needs ffmpeg.
# Usage: sh scripts/gen_pic.sh [out.png]
set -e
cd "$(dirname "$0")/.."
out=${1:-assets/pic0.png}
glow='exp(-(pow(X-1440,2)/(2*520*520)+pow(Y-540,2)/(2*560*560)))'
ffmpeg -v error -y -i assets/brand/logo-src.jpg \
    -f lavfi -i "color=c=black:s=1920x1080,format=rgb24,geq=r='20*$glow':g='30*$glow':b='64*$glow'" \
    -f lavfi -i "color=c=black:s=784x1168,format=gray,geq=lum='clip(255*(136-hypot(max(abs(X-392)-237,0),max(abs(Y-574.5)-404.5,0))),0,255)'" \
    -filter_complex "[0:v]format=rgba[l];[2:v]format=gray[m];[l][m]alphamerge,scale=-2:960[fg];[1:v][fg]overlay=x=1118:y=60:format=auto" \
    -frames:v 1 -pix_fmt rgb24 -compression_level 9 "$out"
