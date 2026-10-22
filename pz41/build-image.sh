#!/bin/bash
PZROOT=${PZROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}; GRAALVM=${GRAALVM:-$HOME/graalvm-25/x64}
# build-image.sh - aarch64 native image of pz41.jar ($PZROOT/pz-switch/build-a64.sh with our jar/config) -> build-a64/pz.o
set -e
B=$PZROOT/pz41/build-a64; rm -rf $B/tmp
B=$B JAR=$PZROOT/pz41/pz41.jar CL=$PZROOT/pz41/config-lua,$PZROOT/pz41/config-res $PZROOT/pz-switch/build-a64.sh -H:TempDirectory=$B/tmp -R:MaxHeapSize=1677721600 -R:MaxNewSize=100663296 -R:+PrintGC -H:+CompactingOldGen -H:MaxSurvivorSpaces=1 -R:MaxHeapFree=67108864 > $PZROOT/pz41/ni-a64.log 2>&1 || { tail -30 $PZROOT/pz41/ni-a64.log; exit 1; }
cp $B/tmp/SVM-*/pz.o $B/pz.o && ls -la $B/pz.o
