#!/bin/bash
PZROOT=${PZROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}; GRAALVM=${GRAALVM:-$HOME/graalvm-25/x64}
# Builds the Java side's real native libraries against musl, one relocatable object per Linux .so name:
#   natives-out/lwjgl.o (+libffi with lx_ffi closures), lwjgl_opengl.o, lwjgl_stb.o
# JNI_OnLoad in each becomes JNI_OnLoad_<lib> so lx_dl's dlsym can tell them apart.
set -e
D=$PZROOT/pz-switch/deps; L=$D/lwjgl3/modules/lwjgl; O=$D/natives-out; G=$GRAALVM
CC="$D/musl-aarch64/bin/musl-gcc"
CFLAGS="-c -std=gnu11 -O2 -fPIE -pthread -DNDEBUG -DLWJGL_LINUX -DLWJGL_arm64 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 \
  -I$G/include -I$G/include/linux -I$L/core/src/main/c -I$L/core/src/main/c/linux -w"
mkdir -p $O && rm -rf $O/tmp && mkdir -p $O/tmp
build() {  # build <lib> <extra flags> <files...>
  local lib=$1 fl=$2; shift 2; local objs=()
  mkdir -p $O/tmp/$lib
  for f in "$@"; do o=$O/tmp/$lib/$(basename "$f" .c).o; $CC $CFLAGS $fl "$f" -o $o & objs+=($o); done
  wait
  aarch64-linux-gnu-ld -r -o $O/$lib.o "${objs[@]}" ${EXTRA_OBJS}
  aarch64-linux-gnu-objcopy --redefine-sym JNI_OnLoad=lxonload_$lib --redefine-sym JNI_OnUnload=lxonunload_$lib $O/$lib.o
  echo "$lib.o: ${#objs[@]} files, undefined: $(aarch64-linux-gnu-nm -u $O/$lib.o | awk '{print $2}' | tr '\n' ' ' | cut -c1-400)"
}
FFI=$D/libffi-3.5.2/aarch64-unknown-linux-gnu
$CC -c -O2 -fPIE $PZROOT/pz-switch/horizon/jside/lx_ffi.c -o $O/tmp/lx_ffi.o
# libffi objects minus closures.o/tramp.o (lx_ffi replaces them)
mkdir -p $O/tmp/ffi && (cd $O/tmp/ffi && ar x $FFI/.libs/libffi.a && rm -f closures.o tramp.o)
EXTRA_OBJS="$O/tmp/lx_ffi.o $(ls $O/tmp/ffi/*.o | tr '\n' ' ')" build lwjgl "-I$L/core/src/main/c/libffi -I$L/core/src/main/c/libffi/aarch64" \
  $L/core/src/main/c/*.c $(ls $L/core/src/generated/c/*.c) $(ls $L/core/src/generated/c/linux/*.c | grep -v liburing)
EXTRA_OBJS= build lwjgl_opengl "-I$L/opengl/src/main/c" $(ls $L/opengl/src/generated/c/*.c | grep -vE 'GLX|WGL')
EXTRA_OBJS= build lwjgl_stb "-I$L/stb/src/main/c -isystem$L/stb/src/main/c" $L/stb/src/generated/c/*.c

# sqlite-jdbc's JNI glue + SQLite. No mmap I/O (MAP_SHARED isn't written back here); exclusive locking keeps a WAL
# index in heap memory instead of a shared mapping; no extension loading.
S=$D/sqlite-jdbc; A=$D/sqlite-amalgamation-3480000
SQLFLAGS="-I$A -I$S/target/common-lib -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_HAVE_ISNAN -DSQLITE_ENABLE_COLUMN_METADATA -DSQLITE_CORE \
  -DSQLITE_ENABLE_FTS3 -DSQLITE_ENABLE_FTS3_PARENTHESIS -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_RTREE -DSQLITE_ENABLE_STAT4 \
  -DSQLITE_ENABLE_DBSTAT_VTAB -DSQLITE_ENABLE_MATH_FUNCTIONS -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
  -DSQLITE_DEFAULT_FILE_PERMISSIONS=0666 -DSQLITE_MAX_VARIABLE_NUMBER=250000 -DSQLITE_MAX_MMAP_SIZE=0 -DSQLITE_MAX_LENGTH=2147483647 \
  -DSQLITE_MAX_COLUMN=32767 -DSQLITE_MAX_SQL_LENGTH=1073741824 -DSQLITE_MAX_FUNCTION_ARG=127 -DSQLITE_MAX_ATTACHED=125 \
  -DSQLITE_MAX_PAGE_COUNT=4294967294 -DSQLITE_DEFAULT_LOCKING_MODE=1"
EXTRA_OBJS= build sqlitejdbc "$SQLFLAGS" $A/sqlite3.c $S/src/main/java/org/sqlite/core/NativeDB.c $PZROOT/pz-switch/horizon/jside/sqlite_noext.c
