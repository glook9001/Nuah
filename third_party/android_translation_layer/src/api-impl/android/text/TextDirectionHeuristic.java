package android.text;

public interface TextDirectionHeuristic {

	public boolean isRtl(CharSequence text, int start, int end);
}
