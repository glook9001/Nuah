package android.media;

import android.view.Display;

public class MediaRouter {
	public static final int ROUTE_TYPE_LIVE_VIDEO = 0x2;

	public static class RouteInfo {
		public Display getPresentationDisplay() {
			return new Display();
		}
	}

	public RouteInfo getSelectedRoute(int type) {
		return new RouteInfo();
	}
}
