package android.widget;

import android.content.Context;
import android.util.AttributeSet;
import android.util.Slog;

public class TextSwitcher extends ViewSwitcher {

	public TextSwitcher(Context context) {
		this(context, null);
	}

	public TextSwitcher(Context context, AttributeSet attrs) {
		super(context, attrs);
	}

	public void setCurrentText(CharSequence text) {
		Slog.v("TextSwitcher", "setCurrentText(" + text + ")");
	}
}
