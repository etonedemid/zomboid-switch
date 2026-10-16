#!/bin/bash
PZROOT=${PZROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}; GRAALVM=${GRAALVM:-$HOME/graalvm-25/x64}
# build-nro.sh - pz41.nro: build 41's native image (build-a64/pz.o, from $PZROOT/pz-switch/build-a64.sh with B/JAR/CL set) linked
# with $PZROOT/pz-switch's Horizon runtime and its LWJGL/sqlite objects. Linux "/" is sdmc:/pz41.
set -e
P=$PZROOT/pz-switch; N=$P/deps/natives-out; O=$PZROOT/pz41/natives-out; M=$P/deps/musl-aarch64/bin/musl-gcc
mkdir -p $O
$M -c -O2 -fPIE $P/horizon/jside/lx_compat.c -o $O/lx_compat.o
python3 $P/gen-natives.py $PZROOT/pz41/pz41.jar $O $N/lwjgl.o $N/lwjgl_opengl.o $N/lwjgl_stb.o $N/sqlitejdbc.o $N/box64.o $O/lx_compat.o --libc=$P/deps/musl-nx-out/lib/libc.a --jmod=$GRAALVM/jmods/java.desktop.jmod
$M -c -O2 -fPIE $O/jni_stubs.S -o $O/jni_stubs.o
$M -c -O2 -fPIE -w $O/dltab.c -o $O/dltab.o
$M -c -O2 -fPIE $P/horizon/jside/lx_dl.c -o $O/lx_dl.o
cd $P
EXTRA_J_LIBS="$O/lx_dl.o $O/dltab.o $O/jni_stubs.o $N/lwjgl.o $N/lwjgl_opengl.o $N/lwjgl_stb.o $N/sqlitejdbc.o $N/box64.o" ./prelink-j.sh $PZROOT/pz41/build-a64/pz.o $PZROOT/pz41/build-a64/j.o
# icon: the game's own 128x128 icon, scaled to the 256x256 JPEG an NRO carries
magick $PZROOT/pz41/game/projectzomboid/projectzomboid.png -filter Lanczos -resize 256x256 -background black -alpha remove -alpha off \
  -quality 95 $PZROOT/pz41/build-a64/icon.jpg
cd horizon/app && rm -f pz41.elf pz41.nro pz41.nacp
make NAME=pz41 ICON=$(realpath --relative-to=. $PZROOT/pz41/build-a64/icon.jpg) TITLE="Project Zomboid 41" BUILD=build41 DEFINES='-DLX_ROOT=\"sdmc:/pz41\"' GL=1 J_OBJ=$PZROOT/pz41/build-a64/j.o \
  EXTRA_LDFLAGS="-Wl,-z,notext -Wl,--wrap=fatalThrow,--wrap=diagAbortWithResult -L$HOME/mesa-switch/libdrm_nouveau/lib" -j8 2>&1 | grep -E 'error|undefined|built'
mv pz41.nro $PZROOT/pz41/pz41.nro && ls -la $PZROOT/pz41/pz41.nro
