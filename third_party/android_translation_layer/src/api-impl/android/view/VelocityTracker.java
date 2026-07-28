package android.view;

public class VelocityTracker {

	public static VelocityTracker obtain() {
		return new VelocityTracker();
	}

	private float startX;
	private float startY;
	private long startEventTime;
	private float currentX;
	private float currentY;
	private long currentEventTime;

	public void addMovement(MotionEvent event) {
		if (event.getActionMasked() == MotionEvent.ACTION_DOWN) {
			startX = currentX = event.getX();
			startY = currentY = event.getY();
			startEventTime = currentEventTime = event.getEventTime();
		} else {
			currentX = event.getX();
			currentY = event.getY();
			currentEventTime = event.getEventTime();
		}
	}

	public void recycle() {}

	public void computeCurrentVelocity(int units, float maxVelocity) {}
	public void computeCurrentVelocity(int units) {}

	public float getXVelocity() {
		return getXVelocity(-1);
	}

	public float getXVelocity(int id) {
		if (currentEventTime == startEventTime)
			return 0.f;
		return (currentX - startX) / (currentEventTime - startEventTime) * 1000;
	}

	public float getYVelocity() {
		return getYVelocity(-1);
	}

	public float getYVelocity(int id) {
		if (currentEventTime == startEventTime)
			return 0.f;
		return (currentY - startY) / (currentEventTime - startEventTime) * 1000;
	}

	public void clear() {}
}
