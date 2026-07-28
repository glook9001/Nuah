package android.view;

import android.view.WindowInsets.Type.InsetsType;

public interface WindowInsetsController {
	void setSystemBarsAppearance(int appearance, int mask);
	void setSystemBarsBehavior(int behavior);
	void show(@InsetsType int types);
	void hide(@InsetsType int types);
}
