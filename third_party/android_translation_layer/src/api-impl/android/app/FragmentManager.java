package android.app;

public class FragmentManager {

	private Activity activity;

	public FragmentManager(Activity activity) {
		this.activity = activity;
	}

	public Fragment findFragmentByTag(String tag) {
		return null;
	}

	public FragmentTransaction beginTransaction() {
		return new FragmentTransaction(activity);
	}

	public boolean executePendingTransactions() {
		return false;
	}
}
