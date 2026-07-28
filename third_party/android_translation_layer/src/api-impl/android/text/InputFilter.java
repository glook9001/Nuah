package android.text;

public interface InputFilter {

	public CharSequence filter(CharSequence source, int start, int end, Spanned dest, int dstart, int dend);

	public static class LengthFilter extends Object implements InputFilter {
		public LengthFilter(int max) {
		}

		public CharSequence filter(CharSequence source, int start, int end, Spanned dest, int dstart, int dend) {
			return "";
		}
	}
}
