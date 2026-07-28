package android.media;

import android.graphics.Bitmap;

public class ThumbnailUtils {

	public static Bitmap extractThumbnail(Bitmap source, int width, int height) {
		return Bitmap.createScaledBitmap(source, width, height, true);
	}
}
