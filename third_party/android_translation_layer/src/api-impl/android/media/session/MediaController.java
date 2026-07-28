package android.media.session;

import android.content.Context;
import android.media.MediaMetadata;

public class MediaController {

	public MediaController(Context context, MediaSession.Token token) {}

	public MediaMetadata getMetadata() {
		return new MediaMetadata();
	}

	public CharSequence getQueueTitle() {
		return null;
	}
}
