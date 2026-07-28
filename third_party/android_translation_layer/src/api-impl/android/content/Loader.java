package android.content;

public class Loader<T> {
	interface OnLoadCompleteListener<D> {
		void onLoadComplete(Loader<D> completedLoader, D data);
	}
}
