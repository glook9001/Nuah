package android.os;

public interface IBinder {

	public interface DeathRecipient {}

	public IInterface queryLocalInterface(String descriptor);

	public boolean transact(int code, Parcel data, Parcel reply, int flags);
}
