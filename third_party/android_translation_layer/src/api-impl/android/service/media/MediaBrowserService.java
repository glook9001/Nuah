package android.service.media;

import android.app.Service;
import android.content.Intent;
import android.media.session.MediaSession;
import android.os.IBinder;

public class MediaBrowserService extends Service {

	@Override
	public IBinder onBind(Intent intent) {
		// TODO Auto-generated method stub
		throw new UnsupportedOperationException("Unimplemented method 'onBind'");
	}

	public void setSessionToken(MediaSession.Token token) {}

	public void notifyChildrenChanged(String parentId) {}
}
