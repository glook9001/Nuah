#include <gtk/gtk.h>
#include <stdio.h>

#include "../defines.h"
#include "../util.h"

#include "WrapperWidget.h"

#include "../generated_headers/android_widget_ProgressBar.h"

JNIEXPORT jlong JNICALL Java_android_widget_ProgressBar_native_1constructor(JNIEnv *env, jobject this, jobject context, jobject attrs)
{
	GtkWidget *wrapper = g_object_ref(wrapper_widget_new());
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkWidget *progress_bar = gtk_progress_bar_new();
	GtkWidget *spinner = gtk_spinner_new();
	wrapper_widget_set_child(WRAPPER_WIDGET(wrapper), box);
	gtk_box_append(GTK_BOX(box), progress_bar);
	gtk_box_append(GTK_BOX(box), spinner);
	gtk_widget_set_name(box, "ProgressBar");
	gtk_widget_set_visible(spinner, FALSE);
	return _INTPTR(box);
}

JNIEXPORT void JNICALL Java_android_widget_ProgressBar_native_1setProgress(JNIEnv *env, jobject this, jlong widget_ptr, jfloat progress)
{
	GtkWidget *box = GTK_WIDGET(_PTR(widget_ptr));
	GtkProgressBar *progress_bar = GTK_PROGRESS_BAR(gtk_widget_get_first_child(box));
	gtk_progress_bar_set_fraction(progress_bar, progress);
}

JNIEXPORT void JNICALL Java_android_widget_ProgressBar_native_1setIndeterminate(JNIEnv *env, jobject this, jboolean indeterminate)
{
	GtkWidget *box = GTK_WIDGET(_PTR(_GET_LONG_FIELD(this, "widget")));
	GtkProgressBar *progress_bar = GTK_PROGRESS_BAR(gtk_widget_get_first_child(box));
	GtkSpinner *spinner = GTK_SPINNER(gtk_widget_get_last_child(box));

	gtk_spinner_set_spinning(spinner, indeterminate);
	gtk_widget_set_visible(GTK_WIDGET(progress_bar), !indeterminate);
	gtk_widget_set_visible(GTK_WIDGET(spinner), indeterminate);
}
