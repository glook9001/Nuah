package android.app;

public class FragmentTransaction {

	private Activity activity;

	public FragmentTransaction(Activity activity) {
		this.activity = activity;
	}

	public FragmentTransaction add(Fragment fragment, String string) {
		fragment.activity = activity;
		activity.fragments.add(fragment);
		return this;
	}

	public int commit() {
		return 0;
	}
}
