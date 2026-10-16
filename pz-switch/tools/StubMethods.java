import java.lang.classfile.*;
import java.lang.constant.ClassDesc;
import java.lang.constant.MethodTypeDesc;
import java.nio.file.*;
import java.util.*;
import java.util.zip.*;

/** StubMethods <in.jar> <out.jar> <list> — rewrites listed methods ("pkg/Class method", one per line) to return a default
 *  value immediately. Used for code paths that can't work on the Switch (e.g. OSHI hardware probing through JNA). */
public class StubMethods {
    public static void main(String[] a) throws Exception {
        Map<String, Set<String>> want = new HashMap<>();
        for (String l : Files.readAllLines(Path.of(a[2]))) {
            l = l.strip();
            if (l.isEmpty() || l.startsWith("#")) continue;
            String[] p = l.split("\\s+");
            want.computeIfAbsent(p[0] + ".class", k -> new HashSet<>()).add(p[1]);
        }
        int done = 0;
        try (ZipInputStream in = new ZipInputStream(Files.newInputStream(Path.of(a[0])));
             ZipOutputStream out = new ZipOutputStream(Files.newOutputStream(Path.of(a[1])))) {
            for (ZipEntry e; (e = in.getNextEntry()) != null; ) {
                byte[] b = in.readAllBytes();
                Set<String> m = want.get(e.getName());
                if (m != null) {
                    ClassFile cf = ClassFile.of();
                    b = cf.transformClass(cf.parse(b), (cb, el) -> {
                        if (el instanceof MethodModel mm && m.contains(mm.methodName().stringValue())) {
                            MethodTypeDesc t = mm.methodTypeSymbol();
                            cb.transformMethod(mm, (mb, me) -> {
                                if (me instanceof CodeModel) mb.withCode(c -> ret(c, t.returnType()));
                                else mb.with(me);
                            });
                        } else cb.with(el);
                    });
                    done += m.size();
                }
                out.putNextEntry(new ZipEntry(e.getName()));
                out.write(b);
            }
        }
        System.out.println("stubbed " + done + " methods");
    }

    static void ret(CodeBuilder c, ClassDesc r) {
        switch (r.descriptorString()) {
            case "V" -> c.return_();
            case "J" -> c.lconst_0().lreturn();
            case "F" -> c.fconst_0().freturn();
            case "D" -> c.dconst_0().dreturn();
            case "Z", "B", "C", "S", "I" -> c.iconst_0().ireturn();
            default -> c.aconst_null().areturn();
        }
    }
}
