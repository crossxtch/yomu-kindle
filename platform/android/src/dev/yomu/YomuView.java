package dev.yomu;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Rect;
import android.view.MotionEvent;
import android.view.View;

import java.lang.reflect.Method;

/* Numbers match AppEvKind in src/ui/app.h. */
final class YomuView extends View {
  static final int EV_TAP = 1;
  static final int EV_SWIPE_LEFT = 2;
  static final int EV_SWIPE_RIGHT = 3;
  static final int EV_SWIPE_UP = 4;
  static final int EV_SWIPE_DOWN = 5;

  /* NTX waveform flags. KOReader sends these for the DPT-RP1: partial GC16
   * is 2, full GC16 is 32 + 2. yomu asks for a waveform on every present.
   */
  private static final int MODE_PARTIAL = 2;
  private static final int MODE_FULL = 34;

  private final YomuActivity activity;
  private final Rect rect = new Rect();
  private final Rect dest = new Rect();
  private Bitmap bitmap;
  private boolean down;
  private int x0;
  private int y0;
  private int panel;
  private Method panelMethod;

  YomuView(YomuActivity activity) {
    super(activity);
    this.activity = activity;
    setBackgroundColor(Color.WHITE);
    setLayerType(LAYER_TYPE_SOFTWARE, null);
  }

  void setBitmap(Bitmap bitmap) {
    this.bitmap = bitmap;
    invalidate();
  }

  @Override
  protected void onDraw(Canvas canvas) {
    if (bitmap == null) {
      return;
    }
    dest.set(0, 0, getWidth(), getHeight());
    canvas.drawBitmap(bitmap, null, dest, null);
  }

  void invalidateEink(int x, int y, int w, int h, boolean full) {
    int mode = full ? MODE_FULL : MODE_PARTIAL;
    int left;
    int top;
    int right;
    int bottom;
    if (panel == 0) {
      probePanel();
    }
    if (w <= 0 || h <= 0 || bitmap == null) {
      rect.set(0, 0, getWidth(), getHeight());
    } else {
      left = scaleX(x);
      top = scaleY(y);
      right = scaleX(x + w);
      bottom = scaleY(y + h);
      if (right <= left) {
        right = left + 1;
      }
      if (bottom <= top) {
        bottom = top + 1;
      }
      rect.set(left, top, right, bottom);
    }
    try {
      if (panel == 1) {
        panelMethod.invoke(this, rect, Integer.valueOf(mode));
        return;
      }
      if (panel == 2) {
        panelMethod.invoke(this, Long.valueOf(0L), Integer.valueOf(rect.left), Integer.valueOf(rect.top),
            Integer.valueOf(rect.width()), Integer.valueOf(rect.height()), Integer.valueOf(mode));
        return;
      }
    } catch (Throwable ignored) {
    }
    invalidate(rect);
  }

  @Override
  public boolean onTouchEvent(MotionEvent event) {
    int action = event.getActionMasked();
    int x = mapX(event.getX());
    int y = mapY(event.getY());
    if (action == MotionEvent.ACTION_DOWN) {
      down = true;
      x0 = x;
      y0 = y;
      return true;
    }
    if (!down) {
      return true;
    }
    if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
      down = false;
      int ms = (int) (event.getEventTime() - event.getDownTime());
      if (ms < 0) {
        ms = 0;
      }
      /* A bottom-edge swipe is the navigation bar's. Finish it anyway when
       * the finger has already moved; a cancelled tap must not turn the page. */
      finishGesture(x0, y0, x, y, ms, action == MotionEvent.ACTION_CANCEL);
      return true;
    }
    return true;
  }

  private void probePanel() {
    panelMethod = findInvalidateMode();
    if (panelMethod != null) {
      panel = 1;
      YomuNative.nativeLog("panel invalidate(Rect, int)");
      return;
    }
    try {
      panelMethod = View.class.getMethod("postInvalidateDelayed", Long.TYPE, Integer.TYPE, Integer.TYPE, Integer.TYPE,
          Integer.TYPE, Integer.TYPE);
      panel = 2;
      YomuNative.nativeLog("panel postInvalidateDelayed");
    } catch (NoSuchMethodException e) {
      panel = 3;
      panelMethod = null;
      YomuNative.nativeLog("panel View.invalidate");
    }
  }

  private Method findInvalidateMode() {
    Class<?> c = getClass();
    while (c != null) {
      try {
        Method method = c.getDeclaredMethod("invalidate", Rect.class, Integer.TYPE);
        method.setAccessible(true);
        return method;
      } catch (NoSuchMethodException e) {
        c = c.getSuperclass();
      }
    }
    return null;
  }

  private int mapX(float x) {
    if (bitmap == null || getWidth() <= 0) {
      return (int) x;
    }
    int v = (int) (x * bitmap.getWidth() / (float) getWidth());
    if (v < 0) {
      return 0;
    }
    if (v >= bitmap.getWidth()) {
      return bitmap.getWidth() - 1;
    }
    return v;
  }

  private int mapY(float y) {
    if (bitmap == null || getHeight() <= 0) {
      return (int) y;
    }
    int v = (int) (y * bitmap.getHeight() / (float) getHeight());
    if (v < 0) {
      return 0;
    }
    if (v >= bitmap.getHeight()) {
      return bitmap.getHeight() - 1;
    }
    return v;
  }

  private int scaleX(int x) {
    if (bitmap == null || getWidth() <= 0 || bitmap.getWidth() <= 0) {
      return x;
    }
    return x * getWidth() / bitmap.getWidth();
  }

  private int scaleY(int y) {
    if (bitmap == null || getHeight() <= 0 || bitmap.getHeight() <= 0) {
      return y;
    }
    return y * getHeight() / bitmap.getHeight();
  }

  private void finishGesture(int sx, int sy, int ex, int ey, int ms, boolean cancel) {
    int width = bitmap != null ? bitmap.getWidth() : getWidth();
    int dx = ex - sx;
    int dy = ey - sy;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int thresh = width / 25;
    int kind;
    if (thresh < 24) {
      thresh = 24;
    }
    if (adx < thresh && ady < thresh) {
      if (cancel) {
        return;
      }
      kind = EV_TAP;
    } else if (adx >= ady) {
      kind = dx < 0 ? EV_SWIPE_LEFT : EV_SWIPE_RIGHT;
    } else {
      kind = dy < 0 ? EV_SWIPE_UP : EV_SWIPE_DOWN;
    }
    /* The reader only accepts a swipe that starts in the bottom eighth.
     * That strip is the navigation bar, so a start in the bottom quarter
     * is reported as the bottom edge. */
    if (kind == EV_SWIPE_UP) {
      sy = liftBottom(sy);
    }
    activity.enqueue(kind, ex, ey, sx, sy, ms);
  }

  private int liftBottom(int sy) {
    int height = bitmap != null ? bitmap.getHeight() : getHeight();
    int band = height / 4;
    if (height <= 1 || sy < height - band) {
      return sy;
    }
    return height - 1;
  }
}
