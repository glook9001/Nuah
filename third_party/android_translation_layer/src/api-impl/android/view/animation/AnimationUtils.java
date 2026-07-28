package android.view.animation;

import android.content.Context;

public class AnimationUtils {
	public static Animation loadAnimation(Context context, int dummy) { return new Animation(); }

	public static long currentAnimationTimeMillis() {
		return System.currentTimeMillis();
	}

	public static Interpolator loadInterpolator(Context context, int dummy) {
		return null;
	}
}
