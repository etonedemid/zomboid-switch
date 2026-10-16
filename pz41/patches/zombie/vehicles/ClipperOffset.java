package zombie.vehicles;

import java.nio.ByteBuffer;

// pz41: libPZClipper64 reads `address` with the jfieldID cached from Clipper,
// so this class must have the same field layout as Clipper (long address; ByteBuffer bb).
public final class ClipperOffset {
    private final long address;
    final ByteBuffer bb = null;

    public ClipperOffset() {
        this.address = this.newInstance();
    }

    private native long newInstance();
    public native void clear();
    public native void addPath(int var1, ByteBuffer var2, int var3, int var4);
    public native void execute(double var1);
    public native int getPolygonCount();
    public native int getPolygon(int var1, ByteBuffer var2);

    public static enum EndType { etClosedPolygon, etClosedLine, etOpenButt, etOpenSquare, etOpenRound }

    public static enum JoinType { jtSquare, jtRound, jtMiter }
}
