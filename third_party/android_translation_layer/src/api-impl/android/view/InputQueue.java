package android.view;

public final class InputQueue {
	// for now, we will put a GtkEventController for the window here
	private long native_ptr = 0;

	public long getNativePtr() {
		return native_ptr; // FIXME?
	}

	public static interface Callback {
		/**
		 * Called when the given InputQueue is now associated with the
		 * thread making this call, so it can start receiving events from it.
		 */
		void onInputQueueCreated(InputQueue queue);

		/**
		 * Called when the given InputQueue is no longer associated with
		 * the thread and thus not dispatching events.
		 */
		void onInputQueueDestroyed(InputQueue queue);
	}
}
