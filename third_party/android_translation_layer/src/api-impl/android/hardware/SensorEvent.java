package android.hardware;

public class SensorEvent {

	public final float[] values;

	public Sensor sensor;

	public SensorEvent(float[] values, Sensor sensor) {
		this.values = values;
		this.sensor = sensor;
	}
}
