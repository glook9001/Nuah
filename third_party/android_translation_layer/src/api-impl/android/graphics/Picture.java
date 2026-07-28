package android.graphics;

public class Picture {

	public Canvas beginRecording(int width, int height) {
		return new Canvas(Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888));
	}

	public void endRecording() {}
}
