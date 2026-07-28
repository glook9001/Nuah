package android.widget;

public interface ListAdapter extends Adapter {

	public boolean isEnabled(int position);

	public boolean areAllItemsEnabled();
}
