package android.graphics.drawable;

import android.graphics.Canvas;
import android.graphics.Paint;

public class ColorDrawable extends Drawable {

	private int color;
	private Paint paint;

	public ColorDrawable(int color) {
		this.color = color;
		this.paint = new Paint();
		this.paint.setColor(color);
	}

	public int getColor() {
		return color;
	}

	@Override
	public void draw(Canvas canvas) {
		canvas.drawRect(getBounds(), paint);
	}

	public void setColor(int color) {
		this.color = color;
		paint.setColor(color);
	}
}
