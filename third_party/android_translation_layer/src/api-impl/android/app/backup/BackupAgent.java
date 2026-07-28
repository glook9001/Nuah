package android.app.backup;

import android.content.ContextWrapper;

public abstract class BackupAgent extends ContextWrapper {
	public BackupAgent() {
		super(null);
	}
}
