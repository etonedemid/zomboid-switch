#!/bin/bash
PZROOT=${PZROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}; GRAALVM=${GRAALVM:-$HOME/graalvm-25/x64}
# build.sh - the whole build 41 NRO: jar -> reflection/JNI config -> aarch64 native image -> pz41.nro
# (pz-switch/setup-deps.sh first; game files in pz41/game/projectzomboid)
set -e
cd $PZROOT/pz41
./make-jar.sh
JAR=$PZROOT/pz41/pz41.jar OUT=$PZROOT/pz41/config-lua NATIVES="$PZROOT/pz41/game/projectzomboid/*.so" python3 $PZROOT/pz-switch/gen-lua-config.py
./build-image.sh
./build-nro.sh
