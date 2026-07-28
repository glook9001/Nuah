#include <gtk/gtk.h>

#include "../defines.h"
#include "../util.h"

#include "../generated_headers/android_content_ClipboardManager.h"

extern GtkWindow *window; // TODO: get this in a better way

JNIEXPORT void JNICALL Java_android_content_ClipboardManager_native_1set_1clipboard(JNIEnv *env, jclass class, jstring text_jstring)
{
	const char *text = (*env)->GetStringUTFChars(env, text_jstring, NULL);
	GdkClipboard *clipboard = gdk_display_get_clipboard(gtk_root_get_display(GTK_ROOT(window)));
	gdk_clipboard_set_text(clipboard, text);
	(*env)->ReleaseStringUTFChars(env, text_jstring, text);
}
