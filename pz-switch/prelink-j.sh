#!/bin/bash
PZROOT=${PZROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}; GRAALVM=${GRAALVM:-$HOME/graalvm-25/x64}
# prelink-j.sh <svm app.o> <out.o> — Java side (SVM image + JDK static libs + musl) into one relocatable object whose only
# global definition is lx_java_entry; everything still undefined must come from the Horizon side.
set -e
IN=$1 OUT=$2; T=$(mktemp -d)
G=$GRAALVM/lib; M=$PZROOT/pz-switch/deps/musl-nx-out
aarch64-linux-gnu-objcopy --redefine-sym main=lx_svm_main "$IN" $T/app.o
for f in lx_entry lx_compat; do $M/../musl-aarch64/bin/musl-gcc -O2 -fPIE -c $PZROOT/pz-switch/horizon/jside/$f.c -o $T/$f.o; done
aarch64-linux-gnu-ld -r --unique=.text* -o $T/j.o $T/lx_entry.o $T/lx_compat.o $T/app.o \
  $G/svm/clibraries/linux-aarch64/glibc/liblibchelper.a $G/static/linux-aarch64/glibc/libprefs.a $G/svm/clibraries/linux-aarch64/glibc/libsvm_container.a \
  $G/static/linux-aarch64/glibc/libextnet.a $G/static/linux-aarch64/glibc/libnio.a $G/static/linux-aarch64/glibc/libnet.a $G/static/linux-aarch64/glibc/libmanagement_ext.a \
  $G/static/linux-aarch64/glibc/libjava.a $G/static/linux-aarch64/glibc/libzip.a $G/svm/clibraries/linux-aarch64/glibc/libjvm.a \
  ${EXTRA_J_LIBS} $M/lib/libz.a $M/lib/libc.a $(aarch64-linux-gnu-gcc -print-libgcc-file-name)
REN=$(aarch64-linux-gnu-readelf -SW $T/j.o | grep -oE '\.(init|fini)_array[.0-9]*' | sort -u | sed -E 's/^\.(init|fini)_array(.*)$/--rename-section .\1_array\2=lx_\1_array/')
# SVM's .rodata holds absolute pointers (relocated at load without SpawnIsolates); Horizon maps .rodata read-only
aarch64-linux-gnu-objcopy $REN --rename-section .rodata=.data.rel.ro,alloc,load,contents,data --redefine-sym __fini_array_start=__start_lx_fini_array --redefine-sym __fini_array_end=__stop_lx_fini_array --keep-global-symbol=lx_java_entry $T/j.o "$OUT"
rm -rf $T
echo "undefined:"; aarch64-linux-gnu-nm -u "$OUT" | awk '{print $2}' | sort | tr '\n' ' '; echo
