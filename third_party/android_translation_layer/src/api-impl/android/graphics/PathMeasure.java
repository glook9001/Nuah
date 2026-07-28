package android.graphics;

public class PathMeasure {

	public PathMeasure() {}

	public PathMeasure(Path path, boolean dummy) {
	}

	public void setPath(Path path, boolean forceClosed) {}

	public float getLength() {
		return 100;
	}

	public boolean getSegment(float start, float end, Path dst, boolean forceClosed) {
		return false;
	}

	public boolean getPosTan(float distance, float[] pos, float[] tan) {
		return false;
	}

	public boolean nextContour() {
		return false;
	}
}
