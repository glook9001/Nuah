package android.content.pm;

import java.util.Collections;
import java.util.List;

public class ShortcutManager {
	public int getMaxShortcutCountPerActivity() { return 4; }
	public void removeAllDynamicShortcuts() {
	}

	public List getShortcuts(int matchFlags) {
		return Collections.emptyList();
	}
	public List getManifestShortcuts() { return Collections.emptyList(); }
	public void removeLongLivedShortcuts(List<String> shortcutIds) {
	}

	public boolean setDynamicShortcuts(List<ShortcutInfo> shortcutInfoList) {
		return true;
	}
}
