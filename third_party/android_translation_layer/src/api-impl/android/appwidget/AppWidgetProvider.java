package android.appwidget;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Slog;

public class AppWidgetProvider extends BroadcastReceiver {

	@Override
	public void onReceive(Context context, Intent intent) {
		Slog.i("AppWidgetProvider", "onReceive(" + intent + ") called");
	}
}
