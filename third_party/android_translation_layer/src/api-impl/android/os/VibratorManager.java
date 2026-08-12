package android.os;

/**
 * Small host implementation of the API 31+ vibrator manager.  Nuah has one
 * logical host haptic device, so the default vibrator is the same object
 * exposed by the legacy "vibrator" service.
 */
public final class VibratorManager {
	private final Vibrator default_vibrator;

	public VibratorManager() {
		default_vibrator = new Vibrator();
	}

	public Vibrator getDefaultVibrator() {
		return default_vibrator;
	}

	public Vibrator getVibrator(int vibratorId) {
		return default_vibrator;
	}

	public int[] getVibratorIds() {
		return new int[] { 0 };
	}

	public Vibrator[] getVibrators() {
		return new Vibrator[] { default_vibrator };
	}
}
