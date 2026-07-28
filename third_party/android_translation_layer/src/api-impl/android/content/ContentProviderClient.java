package android.content;

/** Lightweight in-process provider handle for clients that only need a
 * nullable acquisition result and lifecycle-compatible release methods. */
public class ContentProviderClient implements AutoCloseable {
	private final ContentProvider provider;

	ContentProviderClient(ContentProvider provider) {
		this.provider = provider;
	}

	public ContentProvider getLocalContentProvider() {
		return provider;
	}

	public void close() {}
	public boolean release() { return true; }
}
