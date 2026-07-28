package javax.microedition.khronos.egl;

public class EGLConfig {
	public long native_egl_config = 0;

	public EGLConfig(long native_egl_config) {
		this.native_egl_config = native_egl_config;
	}
}
