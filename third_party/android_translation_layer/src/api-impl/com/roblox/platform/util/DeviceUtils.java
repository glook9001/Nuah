package com.roblox.platform.util;

import android.content.Context;
import android.graphics.Point;
import android.util.DisplayMetrics;

/**
 * Small Android-side display helper used by Roblox's native viewport setup.
 * The desktop host still supplies the authoritative pixel size through ATL's
 * Display implementation; converting those pixels with Android's reported
 * density preserves the API contract without a host-specific JNI shim.
 */
public final class DeviceUtils {
	private DeviceUtils() {}

	public static Point getScreenPhysicalSizeInMillimeters(Context context) {
		DisplayMetrics metrics = null;
		if (context != null && context.getResources() != null)
			metrics = context.getResources().getDisplayMetrics();
		if (metrics == null) {
			metrics = new DisplayMetrics();
			metrics.setToDefaults();
		}

		float xdpi = metrics.xdpi > 0.0f ? metrics.xdpi : DisplayMetrics.DENSITY_DEFAULT;
		float ydpi = metrics.ydpi > 0.0f ? metrics.ydpi : DisplayMetrics.DENSITY_DEFAULT;
		int width = Math.max(1, Math.round(metrics.widthPixels * 25.4f / xdpi));
		int height = Math.max(1, Math.round(metrics.heightPixels * 25.4f / ydpi));
		return new Point(width, height);
	}
}
