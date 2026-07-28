package com.google.android.vending.licensing;

import android.content.Context;

public class LicenseChecker {
	public LicenseChecker(Context context, Policy policy, String encodedPublicKey) {
		// TODO: do something here?
	}

	public synchronized void checkAccess(LicenseCheckerCallback callback) {
		// this is not ideal, but it doesn't make sense to spend much effort on doing this "properly" when the effort required to bypass this doesn't scale with the effort spent by us
		// also, it might not be possible to do this "properly" without having the real google play store in the equation, which is not desirable for a multitude of reasons...
		callback.allow(0x0100 /*Policy.LICENSED*/);
	}
}
