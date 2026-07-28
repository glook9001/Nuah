package android.os;

public class PersistableBundle extends BaseBundle {

	@Override
	public synchronized String toString() {
		return "PersistableBundle[" + mMap.toString() + "]";
	}
}
