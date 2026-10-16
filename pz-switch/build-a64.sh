#!/bin/bash
PZROOT=${PZROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}; GRAALVM=${GRAALVM:-$HOME/graalvm-25/x64}
# native-image build of PZ; extra flags appended from args
B=${B:-$PZROOT/pz-switch/build-a64}; JAR=${JAR:-$PZROOT/pz-switch/pz.jar}; CL=${CL:-$PZROOT/pz-switch/config-lua}
mkdir -p $B && cd $B
exec $GRAALVM/bin/native-image -cp $JAR \
  -H:ConfigurationFileDirectories=$PZROOT/pz-switch/config,$CL --no-fallback -J-Xmx24g \
  --enable-native-access=ALL-UNNAMED --add-exports=java.base/jdk.internal.misc=ALL-UNNAMED \
  --initialize-at-build-time=org.xml.sax.helpers.DefaultHandler$(sed "s/^/,/" $PZROOT/pz-switch/buildtime.txt | tr -d "\n") \
  -H:+ReportExceptionStackTraces -H:+AddAllCharsets "$@" --target=linux-aarch64 -march=compatibility -H:+UnlockExperimentalVMOptions -H:-ForeignAPISupport -H:-MremapImageHeap -H:-SpawnIsolates -H:+UseCAPCache -H:CAPCacheDir=$PZROOT/pz-switch/cap-a64 -H:CCompilerPath=/usr/bin/aarch64-linux-gnu-gcc -H:+ExitAfterRelocatableImageWrite -o pz zombie.gameStates.MainScreenState
