package android.provider;

import android.net.Uri;

public class ContactsContract {

	public static final class CommonDataKinds {

		public static final class Phone {
			public static final Uri CONTENT_URI = Uri.parse("content://com.android.contacts/phones");
		}

		public static final class Email {
			public static final Uri CONTENT_URI = Uri.parse("content://com.android.contacts/emails");
		}
	}

	public static final class Profile {
		public static final Uri CONTENT_URI = Uri.parse("content://com.android.contacts/profile");
	}

	public static final class Contacts {
		public static final Uri CONTENT_URI = Uri.parse("content://com.android.contacts/contacts");
	}

	public static final class RawContacts {
		public static final Uri CONTENT_URI = Uri.parse("content://com.android.contacts/raw_contacts");
	}

	public static final class Data {
		public static final Uri CONTENT_URI = Uri.parse("content://com.android.contacts/data");
	}
}
