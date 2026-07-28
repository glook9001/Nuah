package android.graphics.drawable;

import android.content.res.Resources;
import android.graphics.Rect;

public class InsetDrawable extends DrawableWrapper {

	public InsetDrawable(Drawable drawable, int insetLeft, int insetTop, int insetRight, int insetBottom) {
		super(drawable);
	}

	public InsetDrawable(Drawable drawable, int inset) {
		super(drawable);
	}

	InsetDrawable() {
		super(new InsetState(null, null), null);
	}

	public boolean getPadding(Rect padding) { return false; }

	static final class InsetState extends DrawableWrapper.DrawableWrapperState {

		InsetState(DrawableWrapperState orig, Resources res) {
			super(orig, res);
		}

		@Override
		public Drawable newDrawable(Resources res) {
			// TODO Auto-generated method stub
			throw new UnsupportedOperationException("Unimplemented method 'newDrawable'");
		}
	}
}
