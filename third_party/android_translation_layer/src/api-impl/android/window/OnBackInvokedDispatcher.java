package android.window;

/**
 * Framework back-dispatcher façade.  ATL routes legacy back through
 * Activity.onBackPressed, so registration is intentionally a no-op.
 */
public interface OnBackInvokedDispatcher {
	int PRIORITY_DEFAULT = 0;
	int PRIORITY_OVERLAY = 1_000_000;

	default void registerOnBackInvokedCallback(int priority,
			OnBackInvokedCallback callback) {}

	default void unregisterOnBackInvokedCallback(
			OnBackInvokedCallback callback) {}
}
