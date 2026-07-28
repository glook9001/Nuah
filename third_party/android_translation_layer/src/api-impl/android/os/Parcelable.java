package android.os;

public interface Parcelable {
	public static interface Creator<T> {
		public T createFromParcel(Parcel parcel);

		public T[] newArray(int size);
	}

	public static interface ClassLoaderCreator<T> extends Creator<T> {}

	public default int describeContents() {
		System.out.println("Parcelable.describeContents()");
		return 0;
	}

	public default void writeToParcel(Parcel dest, int flags) {
		System.out.println("Parcelable.writeToParcel(" + this + ", " + dest + ", " + flags + ")");
	}
}
