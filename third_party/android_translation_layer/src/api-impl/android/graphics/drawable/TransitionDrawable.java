package android.graphics.drawable;

import android.graphics.Canvas;

public class TransitionDrawable extends LayerDrawable {

	public TransitionDrawable(Drawable[] layers) {
		super(layers);
	}

	public void setCrossFadeEnabled(boolean enabled) {}

	public void startTransition(int duration) {}

	@Override
	public void draw(Canvas canvas) {
		// always draw the target drawable
		mLayerState.mChildren[1].mDrawable.draw(canvas);
	}
}
