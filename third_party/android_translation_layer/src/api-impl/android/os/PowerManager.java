package android.os;

public final class PowerManager {
	public final class WakeLock {
		public void setReferenceCounted(boolean referenceCounted) {}

		public void acquire() {}

		public void release() {}

		public boolean isHeld() {
			return false;
		}

		public void acquire(long timeout) {}
	}

	public WakeLock newWakeLock(int levelAndFlags, String tag) {
		return new WakeLock();
	}

	public void userActivity(long dummy, boolean dummy2) {}

	public static final int FULL_WAKE_LOCK = 0x1a;

	public boolean isPowerSaveMode() {
		return false;
	}

	public boolean isDeviceIdleMode() {
		return false;
	}

	public boolean isScreenOn() {
		return true;
	}

	public boolean isIgnoringBatteryOptimizations(String packageName) {
		return true;
	}
}
