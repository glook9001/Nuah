package android.app.job;

import android.app.Service;
import android.os.Handler;
import android.os.Looper;
import android.util.Slog;

public abstract class JobService extends Service {
	private static final String TAG = "JobService";

	public abstract boolean onStartJob(JobParameters params);

	public abstract boolean onStopJob(JobParameters params);

	public void jobFinished(JobParameters params, boolean needsReschedule) {
		Slog.i(TAG, "jobFinished(" + params + ", " + needsReschedule + ") called");
		params.jobInfo.running = false;
		if (needsReschedule || params.jobInfo.periodicMillis != 0) {
			new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
				@Override
				public void run() {
					params.jobInfo.running = true;
					boolean result = onStartJob(params);
					Slog.i(TAG, "onStartJob() returned " + result);
				}
			}, needsReschedule ? params.jobInfo.initialBackoffMillis : params.jobInfo.periodicMillis);
		} else {
			JobScheduler.pendingJobs.remove(params.jobInfo.getId());
		}
	}
}
