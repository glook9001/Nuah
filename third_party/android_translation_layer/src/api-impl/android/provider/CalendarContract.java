package android.provider;

import android.net.Uri;

public class CalendarContract {

	public static final class Events {
		public static final Uri CONTENT_URI = Uri.parse("content://com.android.calendar/events");
	}
}
