#include <gdk/gdk.h>
#include <libportal/portal.h>

#include "../defines.h"
#include "../generated_headers/android_app_WallpaperManager.h"

static void wallpaper_ready_cb(GObject *source, GAsyncResult *res, gpointer user_data)
{
	xdp_portal_set_wallpaper_finish(XDP_PORTAL(source), res, NULL);
	GFile *file = user_data;
	g_file_delete(file, NULL, NULL);
	g_object_unref(file);
}

JNIEXPORT void JNICALL Java_android_app_WallpaperManager_set_1bitmap(JNIEnv *env, jclass clazz, jlong texture_ptr)
{
	GdkTexture *texture = _PTR(texture_ptr);
	GFileIOStream *stream;
	GFile *file = g_file_new_tmp("XXXXXX.png", &stream, NULL);
	g_io_stream_close(G_IO_STREAM(stream), NULL, NULL);
	g_object_unref(stream);
	gdk_texture_save_to_png(texture, g_file_get_path(file));
	XdpPortal *portal = xdp_portal_new();
	xdp_portal_set_wallpaper(portal, NULL, g_file_get_uri(file), XDP_WALLPAPER_FLAG_NONE, NULL, wallpaper_ready_cb, file);
	g_object_unref(portal);
}
