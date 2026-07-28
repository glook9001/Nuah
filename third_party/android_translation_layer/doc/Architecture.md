#### directory structure
`doc/` - documentation
`src/api-impl/` - Java code implementing the android APIs
`src/api-impl-jni/` - C code implementing things which it doesn't make sense to do in Java (ideally this would be most things)
`src/libandroid/` - C code implementing `libandroid.so` (this is needed by most JNI libs which come with android apps)
`src/main-executable` - code for the main executable, which sets stuff up (including the JVM) and launches the activity specified on the cmdline
`src/test-runner/` - Java code for test-runner.jar, which contains helpers for instrumentation / testing; taken from AOSP with some changes
`res/framework-res/` - sources for framework-res.apk (taken from AOSP, heavily pruned to remove anything not used by apps)
`res/fonts.xml` - fonts.xml taken from AOSP

#### philosophy

companion infographic for this section:
`doc/android_translation_architecture.svg`

We believe that the cleanest approach for supporting android apps on desktop
Linux platforms is to make a chirurgical cut on the android platform stack
as close to the apps themselves as possible, and sew on a new implementation
of whatever we have cut off.

If you glance at the companion infographic, you can see that the place where
we chose to make the cut is directly between the Apps and the Java APIs
provided by the android frameworks. (additionally, for apps with native
components, we also keep just the respective .so files from those apps
and provide implementations/shims for the system libraries they are linked
against)

#### current control flow (in broad strokes)

1. the user executes the main executable (`android_translation_layer`)

2. the executable is compiled from `src/main-executable/main.c`:
	1. `int main(int argc, char **argv)` sets up a GtkApllication, cmdline handling, and calls `g_application_run`
	2. GtkApplication glue parses the cmdline and calls `static void open(GtkApplication *app, GFile** files, gint nfiles, const gchar* hint, struct jni_callback_data *d)`
	3. `static void open(GtkApplication *app, GFile** files, gint nfiles, const gchar* hint, struct jni_callback_data *d)`:
		1. constructs the classpath from the following:
			- the path to api-impl.jar (contains all the implementations of android framework functions)
			- the path to the app's apk (passed to us on cmdline), making the bytecode within available in classpath
		2. contructs other options (mainly library path) for and launches the dalvik virtual machine
		3. loads `libtranslation_layer_main.so` using the internal version of Runtime.loadLibrary which lets us register this with the right classloader so the java code which declares the native methods is the java code that can use them
		4. sets up a JNI handle cache
		5. sets display size to be passed to apps according to optionally specified window size (some apps handle runtime resizing poorly)
		6. calls `Context.createApplication` to create the `android.app.Application` object for the app (this also parses AndroidManifest.xml)
		7. calls `ContentProvider.createContentProviders` to create content providers specified in the manifest
		8. calls the `OnCreate` method of the application object
		9. calls `Activity.createMainActivity`, passing the class name specified with the `-l` option (if NULL, the main activity as specified in the manifest will be used)
		10. calls `activity_start`, which the `onCreate` method of the activity object
		11. returns to glib main loop - at this point whatever happens is event-driven
