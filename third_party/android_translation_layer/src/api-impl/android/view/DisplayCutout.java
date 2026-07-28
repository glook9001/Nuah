package android.view;

import android.graphics.Insets;

public class DisplayCutout {
	public static final DisplayCutout NO_CUTOUT = new DisplayCutout();

	public Insets getWaterfallInsets() {
		return Insets.NONE;
	}

	public int getSafeInsetBottom() {
		return 0;
	}

	public int getSafeInsetLeft() {
		return 0;
	}

	public int getSafeInsetRight() {
		return 0;
	}

	public int getSafeInsetTop() {
		return 0;
	}
}
