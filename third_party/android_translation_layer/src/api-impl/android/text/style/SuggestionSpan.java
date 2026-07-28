package android.text.style;

import android.content.Context;

public class SuggestionSpan extends CharacterStyle {
	public SuggestionSpan(Context context, String[] suggestions, int flags) {
		System.out.println("SuggestionSpan: flags=" + flags + " suggestions=[" + String.join(", ", suggestions) + "]");
	}
}
