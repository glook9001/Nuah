package android.view;

import android.view.WindowInsets.Type.InsetsType;

public class InsetsController implements WindowInsetsController {
	public void setSystemBarsAppearance(int appearance, int mask) {}
	public void setSystemBarsBehavior(int behavior) {}
	public void show(@InsetsType int types) {}
	public void hide(@InsetsType int types) {}
}
