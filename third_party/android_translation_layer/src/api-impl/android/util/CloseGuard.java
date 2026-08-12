package android.util;

/**
 * Small host implementation used by OkHttp and other Android libraries to
 * report unclosed resources.  ATL does not have Android's VM leak tracker;
 * retaining the same lifecycle methods is enough for callers and avoids a
 * class-linkage failure during Roblox startup.
 */
public final class CloseGuard {
	private String closer;

	public CloseGuard() {}

	public static CloseGuard get() {
		return new CloseGuard();
	}

	public static void setEnabled(boolean enabled) {}

	public static boolean isEnabled() {
		return false;
	}

	public void open(String closer) {
		this.closer = closer;
	}

	public void openWithCallSite(String closer, String callSite) {
		this.closer = closer;
	}

	public void close() {
		closer = null;
	}

	public void warnIfOpen() {
		// Leak diagnostics are intentionally quiet in the host runtime.
	}
}
