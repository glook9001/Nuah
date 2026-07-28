package android.accounts;

import android.content.Context;
import android.os.Bundle;

public class AccountManager {

	public static AccountManager get(Context context) {
		return new AccountManager();
	}

	public Account[] getAccountsByType(String type) {
		return new Account[0];
	}

	public Account[] getAccounts() {
		return new Account[0];
	}

	public boolean addAccountExplicitly(Account account, String password, Bundle extras) {
		System.out.println("addAccountExplicitly(" + account + ", " + password + ", " + extras + ") called");
		return false;
	}
}
