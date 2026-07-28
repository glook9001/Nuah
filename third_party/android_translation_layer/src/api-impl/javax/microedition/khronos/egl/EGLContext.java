package javax.microedition.khronos.egl;

import javax.microedition.khronos.opengles.GL;

public class EGLContext {
	private static final EGL EGL_INSTANCE = new com.google.android.gles_jni.EGLImpl();
	private static final GL GL_INSTANCE = new com.google.android.gles_jni.GLImpl(); // FIXME - not all GLs are created equal
	public long native_egl_context = 0;

	public static EGL getEGL() {
		return EGL_INSTANCE;
	}

	// FIXME - not all GLs are created equal
	public GL getGL() {
		return GL_INSTANCE;
	}

	public EGLContext(long native_egl_context) {
		this.native_egl_context = native_egl_context;
	}
}
