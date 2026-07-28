package android.inputmethodservice;

import android.app.Service;
import android.content.Context;
import android.view.inputmethod.InputMethod;

public abstract class AbstractInputMethodService extends Service {
	public AbstractInputMethodService() {}

	public abstract class AbstractInputMethodImpl implements InputMethod {
		public void createSession(SessionCallback callback) {
		}
	}
}
