#!/bin/bash
PZROOT=${PZROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}; GRAALVM=${GRAALVM:-$HOME/graalvm-25/x64}
# setup-deps.sh - fetches and builds everything in pz-switch/deps: musl (plain, for the Java side's C, and patched for
# Horizon), zlib, libffi, LWJGL 3.4.1, sqlite-jdbc + SQLite, and box64 (zomdroid's, patched), then
# the Java side's native objects (build-natives.sh, build-box64.sh).
set -e
H=$PZROOT/pz-switch/horizon; D=$PZROOT/pz-switch/deps; mkdir -p $D && cd $D
MUSLGCC=$D/musl-aarch64/bin/musl-gcc
get() { [ -f "$(basename $1)" ] || curl -fLO "$1"; }

get https://musl.libc.org/releases/musl-1.2.5.tar.gz
if [ ! -x $MUSLGCC ]; then
  rm -rf musl-1.2.5 && tar xf musl-1.2.5.tar.gz
  (cd musl-1.2.5 && ./configure --target=aarch64 CROSS_COMPILE=aarch64-linux-gnu- --prefix=$D/musl-aarch64 --disable-shared \
     && make -j$(nproc) && make install)
  aarch64-linux-gnu-ar rc musl-aarch64/lib/libatomic_asneeded.a   # GCC 16's specs ask for it; musl needs nothing from it
fi
if [ ! -f musl-nx-out/lib/libc.a ]; then
  rm -rf musl-nx && mkdir musl-nx && tar xf musl-1.2.5.tar.gz -C musl-nx --strip-components=1
  patch -d musl-nx -p1 < $H/musl-nx.patch
  (cd musl-nx && CFLAGS="-fPIE -O2" ./configure --target=aarch64 CROSS_COMPILE=aarch64-linux-gnu- --prefix=$D/musl-nx-out \
     --disable-shared && make -j$(nproc) && make install)
fi

get https://zlib.net/fossils/zlib-1.3.1.tar.gz
if [ ! -f musl-nx-out/lib/libz.a ]; then
  rm -rf zlib-1.3.1 && tar xf zlib-1.3.1.tar.gz
  (cd zlib-1.3.1 && CC=$MUSLGCC CFLAGS="-O2 -fPIE" ./configure --static --prefix=$D/musl-nx-out && make -j$(nproc) libz.a && make install)
fi

get https://github.com/libffi/libffi/releases/download/v3.5.2/libffi-3.5.2.tar.gz
if [ ! -f libffi-3.5.2/aarch64-unknown-linux-gnu/.libs/libffi.a ]; then
  rm -rf libffi-3.5.2 && tar xf libffi-3.5.2.tar.gz
  (cd libffi-3.5.2 && CC=$MUSLGCC CFLAGS="-O2 -fPIE" ./configure --enable-builddir=aarch64-unknown-linux-gnu --host=aarch64-linux-gnu \
     --disable-shared --enable-static --disable-exec-static-tramp --disable-docs && make -j$(nproc))
fi

[ -d lwjgl3 ] || git clone --depth 1 -b 3.4.1 https://github.com/LWJGL/lwjgl3.git

[ -d sqlite-jdbc ] || git clone --depth 1 -b 3.48.0.0 https://github.com/xerial/sqlite-jdbc.git
get https://repo1.maven.org/maven2/org/slf4j/slf4j-api/1.7.36/slf4j-api-1.7.36.jar   # only to compile NativeDB for its JNI header
[ -f sqlite-jdbc/target/common-lib/NativeDB.h ] || (cd sqlite-jdbc && $GRAALVM/bin/javac -nowarn -h target/common-lib \
  -cp ../slf4j-api-1.7.36.jar -d target/hdr-classes -sourcepath src/main/java src/main/java/org/sqlite/core/NativeDB.java \
  && mv target/common-lib/org_sqlite_core_NativeDB.h target/common-lib/NativeDB.h)
get https://www.sqlite.org/2025/sqlite-amalgamation-3480000.zip
[ -d sqlite-amalgamation-3480000 ] || unzip -q sqlite-amalgamation-3480000.zip

if [ ! -d box64 ]; then
  git clone https://github.com/liamelui/zomdroid-box64.git box64
  (cd box64 && git checkout 4a725d54 && git apply $H/box64/box64-musl.patch)
fi
if [ ! -f box64/build-musl-dr/Makefile ]; then
  mkdir -p box64/build-musl-dr && (cd box64/build-musl-dr && cmake .. -DARM64=ON -DARM_DYNAREC=ON -DANDROID=ON -DNOGIT=ON \
    -DBAD_SIGNAL=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
    -DCMAKE_C_COMPILER=$MUSLGCC -DCMAKE_ASM_COMPILER=$MUSLGCC \
    -DCMAKE_C_FLAGS="-fPIE -DBUILD_DYNAMIC -D_GNU_SOURCE -idirafter $H/box64/inc -include $H/box64/musl_compat.h" >/dev/null)
fi

# x86-64 glibc/libstdc++ and zomdroid's JNI wrapper that box64 runs the game's libraries against
get https://raw.githubusercontent.com/liamelui/zomdroid/master/app/src/main/assets/bundles/libs.tar.xz
X=$PZROOT/pz-switch/xfer/x86libs
[ -f $X/_libc.so.6 ] || (mkdir -p $X && tar xJf libs.tar.xz -C $X --strip-components=2 ./linux-x86_64/)

$PZROOT/pz-switch/build-natives.sh
$PZROOT/pz-switch/build-box64.sh
echo "deps ready"
