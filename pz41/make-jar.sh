#!/bin/bash
PZROOT=${PZROOT:-$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)}; GRAALVM=${GRAALVM:-$HOME/graalvm-25/x64}
# make-jar.sh - pz41.jar: Project Zomboid 41.78's classes (loose in the game folder) and its libraries in one jar, with
# two libraries swapped for the versions the Switch runtime already has natives for (from the build 42 game jar):
#   LWJGL 3.4.1 for 3.2.3 (lwjgl.o, lwjgl_opengl.o, lwjgl_stb.o and the GLFW shim are built against 3.4.1),
#   sqlite-jdbc's classes for 3.27.2.1 (sqlitejdbc.o has the newer JNI signatures).
# Then the classes of the mods in mods/*/ on top (performance mods that replace game classes), and patches/.
set -e
cd $PZROOT/pz41
G=game/projectzomboid
B42=${B42JAR:-$HOME/.steam/steam/steamapps/common/ProjectZomboid/projectzomboid/projectzomboid.jar}
rm -rf jar && mkdir jar
for j in commons-compress-1.18 istack-commons-runtime jassimp javacord-2.0.17-shaded javax.activation-api jaxb-api jaxb-runtime \
         trove-3.0.3 uncommons-maths-1.2.3 lwjgl_util; do unzip -qo $G/$j.jar -d jar; done
for d in $(cd $G && find . -maxdepth 1 -type d -not -name . -not -name media -not -name jre64 -not -name linux64 -not -name launcher \
          -not -name Workshop -not -name license -not -name control); do
  find $G/$d -name '*.class' -print -quit | grep -q . && cp -r $G/$d jar/
done
cp $G/stdlib.lbc $G/stdlib.lua $G/serialize.lua jar/   # Lua standard library: classpath resources ("/stdlib.lbc")
rm -rf jar/org/lwjgl jar/org/sqlite jar/META-INF jar/module-info.class
unzip -qo $B42 'org/lwjgl/*' 'org/sqlite/*' -x 'META-INF/*' -d jar
unzip -qo $G/jaxb-runtime.jar "META-INF/services/*" -d jar
unzip -qo $G/sqlite-jdbc-3.27.2.1.jar "META-INF/services/*" -d jar
for m in mods/*/classes; do [ -d "$m" ] && cp -r "$m"/. jar/ && echo "mod: $m"; done
if ls patches/*/ >/dev/null 2>&1; then
  $GRAALVM/bin/javac -nowarn --release 17 -cp jar -d jar $(find patches -name '*.java')
fi
rm -f pz41.jar && (cd jar && zip -qr ../pz41.jar .)
$GRAALVM/bin/java $PZROOT/pz-switch/tools/StubMethods.java pz41.jar pz41-stubbed.jar patches/stub-methods.txt && mv pz41-stubbed.jar pz41.jar
echo "pz41.jar: $(unzip -l pz41.jar | tail -1)"
