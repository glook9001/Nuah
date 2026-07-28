package android.location;

public class Criteria {
	public static final int NO_REQUIREMENT = 0;
	public static final int POWER_LOW = 1;
	public static final int POWER_MEDIUM = 2;
	public static final int POWER_HIGH = 3;
	public static final int ACCURACY_FINE = 1;
	public static final int ACCURACY_COARSE = 2;
	public static final int ACCURACY_LOW = 1;
	public static final int ACCURACY_MEDIUM = 2;
	public static final int ACCURACY_HIGH = 3;

	public void setAccuracy(int accuracy) {}

	public void setAltitudeRequired(boolean required) {}

	public void setBearingRequired(boolean required) {}

	public void setCostAllowed(boolean allowed) {}

	public void setPowerRequirement(int powerRequirement) {}
}
