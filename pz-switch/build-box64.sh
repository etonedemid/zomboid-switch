#!/bin/bash
PZROOT=${PZROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}; GRAALVM=${GRAALVM:-$HOME/graalvm-25/x64}
# box64 (interpreter, musl) + the zomdroid-style JNI bridge -> deps/natives-out/box64.o
set -e
D=$PZROOT/pz-switch/deps; B=$D/box64; O=$D/natives-out; X=$PZROOT/pz-switch/horizon/jside/x86
BD=${BOX64_BUILD:-build-musl-dr}   # build-musl = interpreter only
CC="$D/musl-aarch64/bin/musl-gcc"
make -C $B/$BD -j24 >/dev/null
FL="-O2 -fPIE -D_GNU_SOURCE -DBUILD_DYNAMIC -DANDROID -idirafter $PZROOT/pz-switch/horizon/box64/inc -include $PZROOT/pz-switch/horizon/box64/musl_compat.h \
  -I$B/src/include -I$B/src -I$B/src/emu -I$B/$BD -I$GRAALVM/include -I$GRAALVM/include/linux"
mkdir -p $O/tmp/x86
$CC $FL -c $X/lx_x86.c -o $O/tmp/x86/lx_x86.o
$CC $FL -c $X/wrapped_jni.c -o $O/tmp/x86/wrapped_jni.o
$CC -c $X/lx_x86_tramp.S -o $O/tmp/x86/lx_x86_tramp.o
OBJS=$(find $B/$BD/CMakeFiles -name '*.o' | grep -v 'CMakeC\|/box64gdb\|/src/main.c.o')
aarch64-linux-gnu-ld -r -o $O/box64.o $O/tmp/x86/*.o $OBJS
aarch64-linux-gnu-objcopy --localize-symbol=initialize $O/box64.o
echo "box64.o: $(du -h $O/box64.o | cut -f1), undefined: $(aarch64-linux-gnu-nm -u $O/box64.o | wc -l)"
