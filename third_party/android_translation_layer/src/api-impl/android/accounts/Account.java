package android.accounts;

import android.os.Parcelable;

public class Account implements Parcelable {
	public static final Creator<Account> CREATOR = null;
	public final String name;
	public final String type;

	public Account(String name, String type) {
		this.name = name;
		this.type = type;
	}
}
