package dev.yomu;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.Point;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.Display;
import android.view.View;
import android.view.WindowManager;

import java.io.File;
import java.util.concurrent.CountDownLatch;

public final class YomuActivity extends Activity {
  static final int EV_BACK = 11;
  static final int EV_QUIT = 12;
  static final int EV_WAKE = 16;

  private static final int Q = 32;

  private final Handler ui = new Handler(Looper.getMainLooper());
  private final int[] kinds = new int[Q];
  private final int[] xs = new int[Q];
  private final int[] ys = new int[Q];
  private final int[] x0s = new int[Q];
  private final int[] y0s = new int[Q];
  private final int[] mss = new int[Q];
  private int qh;
  private int qt;
  private int qn;

  private YomuView panel;
  private Bitmap bitmap;
  private WifiManager.MulticastLock multicast;
  private boolean started;
  private boolean sawPause;
  private volatile boolean quitting;

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);
    getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    hideSystemUi();
    panel = new YomuView(this);
    setContentView(panel);
    panel.post(new Runnable() {
      @Override
      public void run() {
        startNative();
      }
    });
  }

  @Override
  protected void onResume() {
    super.onResume();
    hideSystemUi();
    if (sawPause && started && !quitting) {
      sawPause = false;
      enqueue(EV_WAKE, 0, 0, 0, 0, 0);
    }
  }

  @Override
  protected void onPause() {
    sawPause = true;
    super.onPause();
  }

  @Override
  public void onBackPressed() {
    enqueue(EV_BACK, 0, 0, 0, 0, 0);
  }

  @Override
  protected void onDestroy() {
    quitting = true;
    enqueue(EV_QUIT, 0, 0, 0, 0, 0);
    YomuNative.nativeStop();
    if (multicast != null && multicast.isHeld()) {
      multicast.release();
    }
    super.onDestroy();
  }

  public void present(final int x, final int y, final int w, final int h, final int full) {
    if (quitting) {
      return;
    }
    final CountDownLatch latch = new CountDownLatch(1);
    ui.post(new Runnable() {
      @Override
      public void run() {
        try {
          if (!quitting && bitmap != null && panel != null) {
            YomuNative.nativeBlit(x, y, w, h);
            panel.invalidateEink(x, y, w, h, full != 0);
          }
        } finally {
          latch.countDown();
        }
      }
    });
    while (latch.getCount() > 0) {
      try {
        latch.await();
      } catch (InterruptedException e) {
        /* The UI runnable still counts the latch down. */
      }
    }
  }

  public synchronized int poll(int timeoutMs, int[] out) {
    long deadline = timeoutMs > 0 ? SystemClock.uptimeMillis() + timeoutMs : 0;
    while (qn == 0) {
      if (timeoutMs == 0) {
        return 0;
      }
      try {
        if (timeoutMs < 0) {
          wait();
        } else {
          long left = deadline - SystemClock.uptimeMillis();
          if (left <= 0) {
            return 0;
          }
          wait(left);
        }
      } catch (InterruptedException e) {
        return 0;
      }
    }
    out[0] = kinds[qh];
    out[1] = xs[qh];
    out[2] = ys[qh];
    out[3] = x0s[qh];
    out[4] = y0s[qh];
    out[5] = mss[qh];
    qh = (qh + 1) % Q;
    qn--;
    return 1;
  }

  public void onAppExit(final int code) {
    ui.post(new Runnable() {
      @Override
      public void run() {
        YomuNative.nativeLog("activity exit " + code);
        finish();
      }
    });
  }

  synchronized void enqueue(int kind, int x, int y, int x0, int y0, int ms) {
    if (qn == Q) {
      qh = (qh + 1) % Q;
      qn--;
    }
    kinds[qt] = kind;
    xs[qt] = x;
    ys[qt] = y;
    x0s[qt] = x0;
    y0s[qt] = y0;
    mss[qt] = ms;
    qt = (qt + 1) % Q;
    qn++;
    notifyAll();
  }

  private void startNative() {
    int w;
    int h;
    File home;
    File books;
    String open;
    if (started || quitting) {
      return;
    }
    w = panel.getWidth();
    h = panel.getHeight();
    if (w < 16 || h < 16) {
      Display display = getWindowManager().getDefaultDisplay();
      Point size = new Point();
      display.getRealSize(size);
      w = size.x;
      h = size.y;
    }
    if (w < 16 || h < 16) {
      YomuNative.nativeLog("panel size missing");
      finish();
      return;
    }
    bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
    bitmap.eraseColor(Color.WHITE);
    panel.setBitmap(bitmap);
    home = new File(Environment.getExternalStorageDirectory(), "yomu");
    books = new File(home, "books");
    home.mkdirs();
    books.mkdirs();
    open = null;
    if (getIntent() != null && getIntent().getData() != null) {
      open = getIntent().getData().getPath();
    }
    if ((open == null || open.length() == 0) && getIntent() != null) {
      open = getIntent().getStringExtra("path");
    }
    acquireMulticast();
    started = true;
    YomuNative.nativeStart(this, w, h, home.getAbsolutePath(), books.getAbsolutePath(), open, bitmap);
  }

  private void hideSystemUi() {
    View decor = getWindow().getDecorView();
    decor.setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
  }

  private void acquireMulticast() {
    try {
      WifiManager wifi = (WifiManager) getSystemService(WIFI_SERVICE);
      if (wifi == null) {
        return;
      }
      multicast = wifi.createMulticastLock("yomu");
      multicast.setReferenceCounted(false);
      multicast.acquire();
    } catch (RuntimeException e) {
      multicast = null;
    }
  }
}
