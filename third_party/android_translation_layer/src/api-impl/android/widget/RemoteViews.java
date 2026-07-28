package android.widget;

import android.app.PendingIntent;
import android.graphics.Bitmap;

public class RemoteViews {

	public RemoteViews(String packageName, int layoutId) {}

	public void setProgressBar(int viewId, int max, int progress, boolean indeterminate) {}

	public void setTextViewText(int viewId, CharSequence text) {}

	public void setImageViewResource(int viewId, int resId) {}

	public void setContentDescription(int viewId, CharSequence text) {}

	public void setOnClickPendingIntent(int viewId, PendingIntent pendingIntent) {}

	public void setViewVisibility(int viewId, int visibility) {}

	public void setImageViewBitmap(int viewId, Bitmap bitmap) {}

	public void removeAllViews(int viewId) {}

	public RemoteViews clone() {
		return new RemoteViews(null, 0);
	}

	public void addView(int viewId, RemoteViews child) {}

	public void setViewPadding(int viewId, int left, int top, int right, int bottom) {}
}
