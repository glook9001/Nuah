package android.widget;

import android.content.Context;

public class MultiAutoCompleteTextView extends AutoCompleteTextView {

	public static interface Tokenizer {}

	public MultiAutoCompleteTextView(Context context) {
		super(context);
	}

	public void setTokenizer(Tokenizer tokenizer) {}
}
