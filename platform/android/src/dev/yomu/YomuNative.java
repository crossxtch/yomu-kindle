package dev.yomu;

import android.graphics.Bitmap;

public final class YomuNative {
  static {
    System.loadLibrary("yomu");
  }

  private YomuNative() {}

  public static native void nativeStart(YomuActivity activity, int w, int h, String home, String root, String openPath,
      Bitmap bitmap);

  public static native void nativeBlit(int x, int y, int w, int h);

  public static native void nativeStop();

  public static native void nativeLog(String line);
}
