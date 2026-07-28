package android.location;

public final class GnssStatus {
	public static final int CONSTELLATION_UNKNOWN = 0;
	public static final int CONSTELLATION_GPS = 1;
	public static final int CONSTELLATION_SBAS = 2;
	public static final int CONSTELLATION_GLONASS = 3;
	public static final int CONSTELLATION_QZSS = 4;
	public static final int CONSTELLATION_BEIDOU = 5;
	public static final int CONSTELLATION_GALILEO = 6;
	public static final int CONSTELLATION_IRNSS = 7;

	public GnssStatus() {
	}

	public static abstract class Callback {
		public void onStarted() {
		}

		public void onStopped() {
		}

		public void onFirstFix(int ttffMillis) {
		}

		public void onSatelliteStatusChanged(GnssStatus status) {
		}
	}

	public int getSatelliteCount() {
		return 3; // FIXME
	}

	public boolean usedInFix(int index) {
		return true; // FIXME
	}
}
