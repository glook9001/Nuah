package javax.microedition.khronos.egl;

public class EGLDisplay {
	public long native_egl_display = 0;

	public EGLDisplay(long native_egl_display) {
		this.native_egl_display = native_egl_display;
	}
}
