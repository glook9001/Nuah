#include <gtk/gtk.h>
#include <pthread.h>
#include <unistd.h>

#include "../defines.h"
#include "../util.h"

#include "WrapperWidget.h"
#include "marshal.h"

#include "../generated_headers/android_view_SurfaceView.h"
#include "android_view_SurfaceView.h"
#include "../../libandroid/native_window.h"

/* Exported by the standalone ATL executable.  A weak declaration keeps this
 * JNI library usable by applications that do not use Nuah's direct launcher. */
extern void atl_surface_ready(void) __attribute__((weak));
extern GtkWindow *window;

G_DEFINE_TYPE(SurfaceViewWidget, surface_view_widget, GTK_TYPE_WIDGET)

static void surface_view_widget_init(SurfaceViewWidget *surface_view_widget)
{
}

// resize signal copied from GtkDrawingArea
enum {
	RESIZE,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = {
	0,
};

static void surface_view_widget_size_allocate(GtkWidget *widget, int width, int height, int baseline)
{
	g_signal_emit(widget, signals[RESIZE], 0, width, height);
	for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
		gtk_widget_size_allocate(child, &(GtkAllocation){.x = 0, .y = 0, .width = width, .height = height}, baseline);
	}
}

static void surface_view_widget_snapshot(GtkWidget *widget, GdkSnapshot *snapshot)
{
	SurfaceViewWidget *surface_view_widget = SURFACE_VIEW_WIDGET(widget);
#if (GTK_MAJOR_VERSION >= 4 && GTK_MINOR_VERSION >= 22)
	if (getenv("ATL_DIRECT_EGL")) {
		graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, gtk_widget_get_width(widget), gtk_widget_get_height(widget));
		/* the exact color doesn't really matter with GSK_PORTER_DUFF_CLEAR */
		GskRenderNode *hole = gsk_color_node_new(&(GdkRGBA){0, 0, 0, 0}, &bounds);
		GskRenderNode *mask = gsk_color_node_new(&(GdkRGBA){1, 1, 1, 1}, &bounds);
		GskRenderNode *holepunch = gsk_composite_node_new(hole, mask, GSK_PORTER_DUFF_CLEAR);
		gtk_snapshot_append_node(snapshot, holepunch);
		return;
	}
#endif
	if (surface_view_widget->texture) {
		graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, gtk_widget_get_width(widget), gtk_widget_get_height(widget));
		if (surface_view_widget->needs_flip) {
			gtk_snapshot_save(snapshot);
			gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(0, gtk_widget_get_height(widget)));
			gtk_snapshot_scale(snapshot, 1, -1);
		}
		gtk_snapshot_append_texture(snapshot, surface_view_widget->texture, &bounds);
		if (surface_view_widget->needs_flip)
			gtk_snapshot_restore(snapshot);
	}
	if (surface_view_widget->frame_callback) {
		surface_view_widget->frame_callback(surface_view_widget);
		surface_view_widget->frame_callback = NULL;
	}
}

static void surface_view_widget_dispose(GObject *object)
{
	SurfaceViewWidget *surface_view_widget = SURFACE_VIEW_WIDGET(object);
	if (surface_view_widget->texture) {
		g_object_unref(surface_view_widget->texture);
		surface_view_widget->texture = NULL;
	}
	G_OBJECT_CLASS(surface_view_widget_parent_class)->dispose(object);
}

static void surface_view_widget_class_init(SurfaceViewWidgetClass *class)
{
	GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(class);

	// resize signal copied from GtkDrawingArea
	widget_class->size_allocate = surface_view_widget_size_allocate;
	widget_class->snapshot = surface_view_widget_snapshot;
	G_OBJECT_CLASS(class)->dispose = surface_view_widget_dispose;

	signals[RESIZE] =
	    g_signal_new("resize",
	                 G_TYPE_FROM_CLASS(class),
	                 G_SIGNAL_RUN_LAST,
	                 G_STRUCT_OFFSET(GtkDrawingAreaClass, resize),
	                 NULL, NULL,
	                 g_cclosure_user_marshal_VOID__INT_INT,
	                 G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
	g_signal_set_va_marshaller(signals[RESIZE],
	                           G_TYPE_FROM_CLASS(class),
	                           g_cclosure_user_marshal_VOID__INT_INTv);
}

GtkWidget *surface_view_widget_new(void)
{
	return g_object_new(surface_view_widget_get_type(), NULL);
}

void surface_view_widget_set_texture(SurfaceViewWidget *surface_view_widget, GdkTexture *texture, gboolean needs_flip)
{
	if (surface_view_widget->texture)
		g_object_unref(surface_view_widget->texture);
	surface_view_widget->texture = texture;
	surface_view_widget->needs_flip = needs_flip;
	gtk_widget_queue_draw(GTK_WIDGET(surface_view_widget));
}

// ---

struct jni_callback_data {
	JavaVM *jvm;
	jobject this;
	jclass this_class;
	GtkWidget *surface_view_widget;
	gint resize_width;
	gint resize_height;
	gboolean surface_created;
	gboolean surface_changed;
	guint surface_ready_attempts;
	gboolean late_dispatch_started;
	gboolean surface_replay_queued;
	jclass game_activity_class;
	jfieldID native_handle;
	struct ANativeWindow *native_window;
	gboolean native_window_notified;
};

static void ensure_native_window(struct jni_callback_data *d, GtkWidget *widget)
{
	if (d->native_window)
		return;
	GtkWidget *parent = gtk_widget_get_parent(widget);
	d->native_window = ANativeWindow_fromSurface(NULL, (jobject)parent);
}

static void dispatch_activity_surface(struct jni_callback_data *d, JNIEnv *env,
	                                  gint width, gint height)
{
	if (!d->game_activity_class || !d->native_handle)
		return;
	jlong native_handle = (*env)->GetStaticLongField(env, d->game_activity_class,
	                                                  d->native_handle);
	/* The realize signal can precede GameActivity.initializeNativeCode. */
	if (native_handle == 0)
		return;
	(void)native_handle;
	/* SurfaceView.post() queues the callbacks on the Java UI thread.  Calling
	 * GameActivity's native methods directly from the polling thread violates
	 * ART's thread state contract and corrupts Roblox's render state. */
	jmethodID dispatch = (*env)->GetMethodID(env, d->this_class,
		"dispatchSurfaceLifecycle", "(II)V");
	if (dispatch && !d->native_window_notified) {
		d->native_window_notified = TRUE;
		(*env)->CallVoidMethod(env, d->this, dispatch, width, height);
	}
	if ((*env)->ExceptionCheck(env))
		(*env)->ExceptionClear(env);
}

static gboolean surface_replay_on_main(gpointer opaque)
{
	struct jni_callback_data *d = opaque;
	d->surface_replay_queued = FALSE;
	JNIEnv *env = NULL;
	jboolean detach = JNI_FALSE;
	if ((*d->jvm)->GetEnv(d->jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
		if ((*d->jvm)->AttachCurrentThread(d->jvm, (void **)&env, NULL) != JNI_OK)
			return G_SOURCE_REMOVE;
		detach = JNI_TRUE;
	}
	d->surface_created = TRUE;
	const gint width = d->resize_width > 0 ? d->resize_width : 1280;
	const gint height = d->resize_height > 0 ? d->resize_height : 720;
	dispatch_activity_surface(d, env, width, height);
	d->surface_changed = TRUE;
	if (detach)
		(*d->jvm)->DetachCurrentThread(d->jvm);
	return G_SOURCE_REMOVE;
}

static void *late_surface_dispatch(void *opaque)
{
	struct jni_callback_data *d = opaque;
	/* GameActivity installs its SurfaceHolder callbacks asynchronously.  A
	 * single replay can land before that registration; retry for a bounded
	 * startup window so we don't make SurfaceView lifecycle timing fatal. */
	for (int attempt = 0; attempt < 10; ++attempt) {
		g_usleep(100000);
		/* This worker only invokes SurfaceView.post(), which transfers the
		 * actual callbacks to Java's main Handler.  Do not call Roblox's native
		 * lifecycle methods from here. */
		if (!d->native_window_notified) {
			JNIEnv *env = NULL;
			jboolean detach = JNI_FALSE;
			if ((*d->jvm)->GetEnv(d->jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK &&
			    (*d->jvm)->AttachCurrentThread(d->jvm, (void **)&env, NULL) == JNI_OK)
				detach = JNI_TRUE;
			if (env) {
				d->surface_created = TRUE;
				dispatch_activity_surface(d, env, 1280, 720);
				d->surface_changed = TRUE;
			}
			if (detach)
				(*d->jvm)->DetachCurrentThread(d->jvm);
		}
		if (d->native_window_notified && d->surface_changed)
			return NULL;
	}
	return NULL;
}

static gboolean dispatch_surface_changed(struct jni_callback_data *d)
{
	if (!d->surface_created)
		return G_SOURCE_CONTINUE;

	const gint width = d->resize_width > 0 ? d->resize_width :
		gtk_widget_get_width(d->surface_view_widget);
	const gint height = d->resize_height > 0 ? d->resize_height :
		gtk_widget_get_height(d->surface_view_widget);
	if (width <= 0 || height <= 0) {
		/* GTK may realize the widget before its first allocation.  Keep
		 * polling briefly instead of allowing Roblox to observe a null
		 * ANativeWindow and permanently discard the surface. */
		if (++d->surface_ready_attempts < 300)
			return G_SOURCE_CONTINUE;
		return G_SOURCE_REMOVE;
	}

	d->resize_width = width;
	d->resize_height = height;
	JNIEnv *env;
	(*d->jvm)->GetEnv(d->jvm, (void **)&env, JNI_VERSION_1_6);
	dispatch_activity_surface(d, env, width, height);
	d->surface_changed = TRUE;
	return G_SOURCE_REMOVE;
}

static gboolean on_resize_delayed(struct jni_callback_data *d)
{
	if (d->surface_changed)
		return G_SOURCE_REMOVE;
	return dispatch_surface_changed(d);
}

static void on_resize(GtkWidget *self, gint width, gint height, struct jni_callback_data *d)
{
	d->resize_width = width;
	d->resize_height = height;
	/* A size allocation is the earliest reliable point at which the activity
	 * has a real window.  Use it to release the direct-launch bridge even if
	 * the realize signal was consumed by a parent offload widget. */
	JNIEnv *env = NULL;
	if (atl_surface_ready &&
	    (*d->jvm)->GetEnv(d->jvm, (void **)&env, JNI_VERSION_1_6) == JNI_OK &&
	    d->game_activity_class && d->native_handle &&
	    (*env)->GetStaticLongField(env, d->game_activity_class,
                               d->native_handle) != 0)
		atl_surface_ready();

	/* A high-priority idle can be starved by GTK's continuous frame work while
	 * Roblox is booting.  Use a short timer so the Java surface callback is
	 * delivered even during that redraw burst. */
	g_timeout_add_full(G_PRIORITY_DEFAULT, 1, G_SOURCE_FUNC(on_resize_delayed), d, NULL);
}

static gboolean on_realize_delayed(struct jni_callback_data *d)
{
	JNIEnv *env;
	(*d->jvm)->GetEnv(d->jvm, (void **)&env, JNI_VERSION_1_6);

	d->surface_created = TRUE;

	/* The first size allocation can precede GTK realization.  In that case the
	 * resize signal records dimensions but its Java callback is never delivered,
	 * leaving Roblox's SurfaceController with a null native window. */
	d->surface_ready_attempts = 0;
	if (dispatch_surface_changed(d) != G_SOURCE_REMOVE)
		g_timeout_add_full(G_PRIORITY_HIGH_IDLE + 20, 16,
		                   G_SOURCE_FUNC(dispatch_surface_changed), d, NULL);

	return G_SOURCE_REMOVE;
}

static void on_realize(GtkWidget *self, struct jni_callback_data *d)
{
	if (gtk_widget_get_width(self) <= 0 || gtk_widget_get_height(self) <= 0)
		gtk_widget_size_allocate(self,
		                         &(GtkAllocation){.x = 0, .y = 0,
		                                           .width = 1280, .height = 720}, -1);
	JNIEnv *env = NULL;
	jboolean detach = JNI_FALSE;
	if ((*d->jvm)->GetEnv(d->jvm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
		if ((*d->jvm)->AttachCurrentThread(d->jvm, (void **)&env, NULL) != JNI_OK)
			return;
		detach = JNI_TRUE;
	}
	/* Build the GTK-backed native window while this signal is on GTK's UI
	 * thread.  The later GameActivity replay only publishes this object. */
	ensure_native_window(d, self);
	if (detach)
		(*d->jvm)->DetachCurrentThread(d->jvm);
	/* The ATL launcher may run GTK from a non-default GLib context, where
	 * g_idle/g_timeout sources never execute.  The realize signal itself is on
	 * GTK's UI thread, so deliver the Android surface callbacks synchronously. */
	on_realize_delayed(d);
	/* GameActivity may register its native callbacks after the first GTK
	 * realization.  Replay the lifecycle once registration has completed. */
	if (!d->late_dispatch_started) {
		d->late_dispatch_started = TRUE;
		pthread_t thread;
		if (pthread_create(&thread, NULL, late_surface_dispatch, d) == 0)
			pthread_detach(thread);
	}
}

JNIEXPORT jlong JNICALL Java_android_view_SurfaceView_native_1constructor(JNIEnv *env, jobject this, jobject context, jobject attrs)
{
	GtkWidget *wrapper = g_object_ref(wrapper_widget_new());
	GtkWidget *dummy = surface_view_widget_new();
	gtk_widget_set_name(dummy, "dummy widget for SurfaceView");
	/* The Android SurfaceView has a real buffer size before the first
	 * SurfaceHolder callback.  Give GTK the same host dimensions so
	 * ANativeWindow_fromSurface cannot wait forever for its allocation. */
	gtk_widget_set_size_request(dummy, 1280, 720);
	GtkWidget *graphics_offload = gtk_graphics_offload_new(dummy);
	gtk_widget_set_hexpand(graphics_offload, true);
	gtk_widget_set_vexpand(graphics_offload, true);
	gtk_widget_set_hexpand(dummy, true);
	gtk_widget_set_vexpand(dummy, true);
	gtk_widget_set_visible(dummy, TRUE);
	gtk_widget_set_visible(graphics_offload, TRUE);
	wrapper_widget_set_child(WRAPPER_WIDGET(wrapper), graphics_offload);
	wrapper_widget_set_jobject(WRAPPER_WIDGET(wrapper), env, this);
	// TODO: is this correct for all usecases? how do we know when it's not?
	gtk_widget_set_hexpand(wrapper, true);
	gtk_widget_set_vexpand(wrapper, true);
	gtk_widget_set_visible(wrapper, TRUE);
	wrapper_widget_set_layout_params(WRAPPER_WIDGET(wrapper), 1280, 720);

	JavaVM *jvm;
	(*env)->GetJavaVM(env, &jvm);

	struct jni_callback_data *callback_data = malloc(sizeof(struct jni_callback_data));
	callback_data->jvm = jvm;
	callback_data->this = _REF(this);
	callback_data->this_class = _REF((*env)->FindClass(env, "android/view/SurfaceView"));
	callback_data->surface_view_widget = dummy;
	callback_data->surface_created = FALSE;
	callback_data->surface_changed = FALSE;
	callback_data->surface_ready_attempts = 0;
	callback_data->late_dispatch_started = FALSE;
	jmethodID get_context = (*env)->GetMethodID(env, callback_data->this_class,
	                                            "getContext", "()Landroid/content/Context;");
	jobject view_context = (*env)->CallObjectMethod(env, this, get_context);
	jclass activity_class = (*env)->GetObjectClass(env, view_context);
	callback_data->game_activity_class = NULL;
	callback_data->native_handle = NULL;
	/* ActivityNativeMain also owns an ordinary preview SurfaceView, while
	 * MainGameActivity inherits GameActivity and exposes its static P handle.
	 * Walk the hierarchy instead of assuming the immediate superclass is
	 * GameActivity; a missing P is valid for the preview and must not leave a
	 * pending NoSuchFieldError in the ART VM. */
	for (jclass probe = activity_class; probe; ) {
		jfieldID native_handle = (*env)->GetStaticFieldID(env, probe, "P", "J");
		if (!native_handle) {
			(*env)->ExceptionClear(env);
			jclass parent = (*env)->GetSuperclass(env, probe);
			if (probe != activity_class) (*env)->DeleteLocalRef(env, probe);
			probe = parent;
			continue;
		}
		callback_data->game_activity_class = (*env)->NewGlobalRef(env, probe);
		callback_data->native_handle = native_handle;
		if (probe != activity_class) (*env)->DeleteLocalRef(env, probe);
		break;
	}
	(*env)->DeleteLocalRef(env, activity_class);
	(*env)->DeleteLocalRef(env, view_context);
	callback_data->native_window = NULL;
	callback_data->native_window_notified = FALSE;

	g_signal_connect(dummy, "resize", G_CALLBACK(on_resize), callback_data);
	g_signal_connect(dummy, "realize", G_CALLBACK(on_realize), callback_data);
	/* Start replay even when the host loop does not emit GTK realize. */
	if (!callback_data->late_dispatch_started) {
		callback_data->late_dispatch_started = TRUE;
		pthread_t thread;
		if (pthread_create(&thread, NULL, late_surface_dispatch, callback_data) == 0)
			pthread_detach(thread);
	}

	return _INTPTR(graphics_offload);
}

JNIEXPORT void JNICALL Java_android_view_SurfaceView_nativeDirectAppStart(JNIEnv *env, jclass klass)
{
	(void)env;
	(void)klass;
	if (atl_surface_ready)
		atl_surface_ready();
}

JNIEXPORT jlong JNICALL Java_android_view_SurfaceView_native_1createSnapshot(JNIEnv *env, jclass class)
{
	return _INTPTR(gtk_snapshot_new());
}

JNIEXPORT void JNICALL Java_android_view_SurfaceView_native_1postSnapshot(JNIEnv *env, jclass class, jlong surface_view, jlong snapshot_ptr)
{
	GtkWidget *view = GTK_WIDGET(_PTR(surface_view));
	SurfaceViewWidget *surface_view_widget = SURFACE_VIEW_WIDGET(gtk_widget_get_first_child(view));
	GtkSnapshot *snapshot = GTK_SNAPSHOT(_PTR(snapshot_ptr));
	static GType renderer_type = 0;
	if (!renderer_type) {
		// Use same renderer type as for onscreen rendering.
		GdkSurface *surface = gdk_surface_new_toplevel(gdk_display_get_default());
		GskRenderer *renderer = gsk_renderer_new_for_surface(surface);
		renderer_type = G_OBJECT_TYPE(renderer);
		gsk_renderer_unrealize(renderer);
		g_object_unref(renderer);
		gdk_surface_destroy(surface);
	}
	GskRenderer *renderer = g_object_new(renderer_type, NULL);
	gsk_renderer_realize(renderer, NULL, NULL);
	GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
	GdkTexture *texture = gsk_renderer_render_texture(renderer, node, NULL);
	gsk_render_node_unref(node);
	gsk_renderer_unrealize(renderer);
	g_object_unref(renderer);

	surface_view_widget_set_texture(surface_view_widget, texture, FALSE);
}
