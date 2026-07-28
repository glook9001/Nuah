package android.speech.tts;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

public class TextToSpeech {
	public static final int ERROR = -1;

	public TextToSpeech(Context context, TextToSpeech.OnInitListener listener) {
		new Handler(Looper.getMainLooper()).post(new Runnable() {
			@Override
			public void run() {
				listener.onInit(ERROR);
			}
		});
	}

	public int setOnUtteranceCompletedListener(TextToSpeech.OnUtteranceCompletedListener listener) {
		return ERROR;
	}

	public int setOnUtteranceProgressListener(UtteranceProgressListener listener) {
		return ERROR;
	}

	public void shutdown() {
	}

	public int stop() {
		return ERROR;
	}

	public static interface OnInitListener {
		abstract void onInit(int status);
	}

	public static interface OnUtteranceCompletedListener {
		public abstract void onUtteranceCompleted(String utteranceId);
	}
}
