package android.app;

import android.atl.ATLLoadedApp;
import android.content.Context;
import android.content.pm.ConfigurationInfo;
import android.graphics.Bitmap;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.Process;
import java.io.BufferedReader;
import java.io.FileReader;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

public class ActivityManager {
	/* Android reports a foreground process with importance 100.  Leaving the
	 * field at Java's default zero makes applications (including Roblox) treat
	 * the ATL process as a background worker and stop before creating an
	 * Activity. */
	public static final int IMPORTANCE_FOREGROUND = 100;
	public static final int IMPORTANCE_BACKGROUND = 400;

	public static class RunningAppProcessInfo {
		public int importance;
		public int pid;
		public int uid;
		public String processName;
		public String[] pkgList;

		private RunningAppProcessInfo(int pid, String processName) {
			this.pid = pid;
			this.uid = Process.myUid();
			this.processName = processName;
			this.pkgList = new String[] {processName};
			this.importance = IMPORTANCE_FOREGROUND;
		}
	}

	public static class TaskDescription {
		public TaskDescription(String name) {}
		public TaskDescription(String name, Bitmap icon, int color) {}
	}

	public List<RunningAppProcessInfo> getRunningAppProcesses() {
		return Arrays.asList(new RunningAppProcessInfo(Process.myPid(),
		                                               ATLLoadedApp.getPrimaryApplication().pkg.packageName));
	}

	public boolean isLowRamDevice() { return false; }

	public static class MemoryInfo {
		/* Values are bytes, as in the Android API.  Keep the object mutable: the
		 * caller passes this instance to getMemoryInfo(). */
		public long availMem = 0;
		public long totalMem = 0;
		public long threshold = 0;

		public boolean lowMemory = false;

		public int describeContents() { return 0; }
		public void writeToParcel(Parcel dest, int flags) {}
		public void readFromParcel(Parcel source) {}
	}

	private static long procMemBytes(String name, long fallback) {
		BufferedReader reader = null;
		try {
			reader = new BufferedReader(new FileReader("/proc/meminfo"));
			String line;
			while ((line = reader.readLine()) != null) {
				if (!line.startsWith(name + ":")) continue;
				int colon = line.indexOf(':');
				String value = line.substring(colon + 1).trim();
				int end = 0;
				while (end < value.length() && value.charAt(end) >= '0' &&
				       value.charAt(end) <= '9') end++;
				if (end == 0) break;
				return Long.parseLong(value.substring(0, end)) * 1024L;
			}
		} catch (Exception ignored) {
			// A restricted runtime may not expose /proc; use the conservative
			// desktop profile below in that case.
		} finally {
			if (reader != null) {
				try { reader.close(); } catch (Exception ignored) {}
			}
		}
		return fallback;
	}

	public void getMemoryInfo(MemoryInfo outInfo) {
		if (outInfo == null) return;
		long total = procMemBytes("MemTotal", 4L * 1024L * 1024L * 1024L);
		long available = procMemBytes("MemAvailable", total / 2L);
		if (available < 0) available = 0;
		if (available > total) available = total;
		outInfo.totalMem = total;
		outInfo.availMem = available;
		outInfo.threshold = Math.max(64L * 1024L * 1024L, total / 10L);
		outInfo.lowMemory = available < outInfo.threshold;
	}

	public ConfigurationInfo getDeviceConfigurationInfo() {
		return new ConfigurationInfo();
	}

	public int getMemoryClass() { return 256; }      // Nuah device profile, in MB
	public int getLargeMemoryClass() { return 512; } // Nuah large-heap profile, in MB

	public static void getMyMemoryState(RunningAppProcessInfo outInfo) {
		if (outInfo != null) outInfo.importance = IMPORTANCE_FOREGROUND;
	}

	public boolean clearApplicationUserData() { return false; }

	public static class AppTask {}
	public List<ActivityManager.AppTask> getAppTasks() {
		return new ArrayList<>();
	}

	public static class RunningServiceInfo implements Parcelable {
		public RunningServiceInfo() {
		}

		public int describeContents() {
			return 0;
		}

		public void writeToParcel(Parcel dest, int flags) {
			return;
		}

		public void readFromParcel(Parcel source) {
			return;
		}
	}

	public List<RunningServiceInfo> getRunningServices(int maxNum)
	    throws SecurityException {
		return new ArrayList<>();
	}

	public List<ApplicationExitInfo> getHistoricalProcessExitReasons(String pkgname, int pid, int maxNum) {
		return Collections.emptyList();
	}

	public static boolean isUserAMonkey() { return false; }

	public void moveTaskToFront(int taskId, int flags, Bundle options) {
	}
}
