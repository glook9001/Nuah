package android.text.method;

import android.text.Spannable;
import android.view.MotionEvent;
import android.widget.TextView;

public class BaseMovementMethod implements MovementMethod {

	public boolean onTouchEvent(TextView widget, Spannable buffer, MotionEvent event) {
		return false;
	}
}
