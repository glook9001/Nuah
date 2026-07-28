package android.text;
/**
 * @deprecated Old text-only interface to the clipboard.  See
 * {@link android.content.ClipboardManager} for the modern API.
 */
@Deprecated
public abstract class ClipboardManager {
	/**
	 * Returns the text on the clipboard.  It will eventually be possible
	 * to store types other than text too, in which case this will return
	 * null if the type cannot be coerced to text.
	 */
	public abstract CharSequence getText();
	/**
	 * Sets the contents of the clipboard to the specified text.
	 */
	public abstract void setText(CharSequence text);
	/**
	 * Returns true if the clipboard contains text; false otherwise.
	 */
	public abstract boolean hasText();
}
