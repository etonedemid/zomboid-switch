#!/usr/bin/env python3
# Registers every public member Kahlua can reach: classes from LuaManager$Exposer.exposeAll() plus all of zombie.* and se.krka.*.
import json, re, subprocess, zipfile, os
os.environ.setdefault('PZROOT', os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ.setdefault('GRAALVM', os.path.expanduser('~/graalvm-25/x64'))
ex = lambda p: os.path.expandvars(os.path.expanduser(p))
jar = ex(os.environ.get('JAR', '$PZROOT/pz-switch/pz.jar'))
out_json = ex(os.environ.get('OUT', '$PZROOT/pz-switch/config-lua') + '/reachability-metadata.json')
javap = ex('$GRAALVM/bin/javap')
out = subprocess.run([javap, '-p', '-c', '-cp', jar, 'zombie.Lua.LuaManager$Exposer'], capture_output=True, text=True).stdout
names = {m.replace('/', '.') for m in re.findall(r'ldc\s+#\d+\s+// class (\S+)', out)}
for n in zipfile.ZipFile(jar).namelist():
    if n.endswith('.class') and n.startswith(('zombie/', 'se/krka/')) and 'module-info' not in n:
        names.add(n[:-6].replace('/', '.'))
names = sorted(n for n in names if not n.startswith('['))
refl = [{"type": n, "allPublicMethods": True, "allPublicFields": True, "allPublicConstructors": True, "allDeclaredMethods": True, "allDeclaredFields": True, "allDeclaredConstructors": True} for n in names]
# Array.newInstance on game types (e.g. generic pools)
refl += [{"type": n + "[]"} for n in names]
json.dump({"reflection": refl}, open(out_json, 'w'), indent=1)
print(len(refl), 'types')

# JNI: every class named in the natives (FindClass strings) or declaring Java_ exports gets all declared members, JNI-accessible.
import glob
nat = glob.glob(ex(os.environ.get('NATIVES', '~/.steam/steam/steamapps/common/ProjectZomboid/projectzomboid/natives/*.so')))
jni = set()
for s in subprocess.run(['strings', '-n', '4'] + nat, capture_output=True, text=True).stdout.split():
    m = re.match(r'^L?((?:zombie|fmod|jassimp|imgui|se/krka|java/(?:lang|util|nio|io))/[A-Za-z0-9_/$]+)', s)
    if m: jni.add(m.group(1).replace('/', '.'))
for l in subprocess.run(['nm', '-D', '--defined-only'] + nat, capture_output=True, text=True).stdout.split('\n'):
    m = re.search(r' Java_(\w+)', l)
    if not m: continue
    mangled = m.group(1).split('__')[0].replace('_1', '\x01').replace('_00024', '\x02')
    jni.add('.'.join(mangled.split('_')[:-1]).replace('\x01', '_').replace('\x02', '$'))
classes = set(n[:-6].replace('/', '.') for n in zipfile.ZipFile(jar).namelist() if n.endswith('.class'))
# JDK classes the natives reach through GetObjectClass/GetMethodID on objects they're handed
jni |= {'java.nio.Buffer', 'java.nio.ByteBuffer', 'java.nio.DirectByteBuffer', 'java.nio.DirectByteBufferR', 'java.nio.HeapByteBuffer',
        'java.nio.FloatBuffer', 'java.nio.DirectFloatBufferU', 'java.nio.IntBuffer', 'java.nio.DirectIntBufferU', 'java.nio.ShortBuffer',
        'java.nio.DirectShortBufferU', 'java.nio.LongBuffer', 'java.nio.DoubleBuffer', 'java.nio.CharBuffer', 'java.lang.String',
        'java.lang.Object', 'java.lang.Class', 'java.lang.Throwable', 'java.lang.RuntimeException', 'java.lang.Integer',
        'java.lang.Float', 'java.lang.Long', 'java.lang.Double', 'java.lang.Boolean', 'java.util.ArrayList', 'java.util.HashMap'}
jni = sorted(c for c in jni if c in classes or c.startswith('java.'))
by = {e['type']: e for e in refl}
for c in jni:
    e = by.setdefault(c, {"type": c})
    e.update({"jniAccessible": True, "allDeclaredMethods": True, "allDeclaredFields": True, "allDeclaredConstructors": True})
json.dump({"reflection": list(by.values())}, open(out_json, 'w'), indent=1)
print(len(jni), 'jni types')
