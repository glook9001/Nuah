package android.os;

public abstract class CountDownTimer {
	public CountDownTimer(long millisInFuture, long countDownInterval) {}

	public final void cancel() {}
	public abstract void onFinish();
	public abstract void onTick(long millisUntilFinished);
	public final CountDownTimer start() {
		return this;
	}
}
