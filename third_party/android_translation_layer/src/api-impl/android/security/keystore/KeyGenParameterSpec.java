package android.security.keystore;

import java.security.spec.AlgorithmParameterSpec;
import java.util.Date;

public class KeyGenParameterSpec {

	private String keystoreAlias;
	private int purposes;
	private int keySize;
	private String[] blockModes;
	private String[] encryptionPaddings;
	private String[] digests;
	private byte[] attestationChallenge;
	private Date keyValidityStart;
	private boolean isStrongBoxBacked;
	private boolean userAuthenticationRequired;
	private AlgorithmParameterSpec algorithmParameterSpec;

	public static class Builder {
		private KeyGenParameterSpec spec = new KeyGenParameterSpec();

		public Builder(String keystoreAlias, int purposes) {
			spec.keystoreAlias = keystoreAlias;
			spec.purposes = purposes;
		}

		public Builder setKeySize(int keySize) {
			spec.keySize = keySize;
			return this;
		}

		public Builder setBlockModes(String[] blockModes) {
			spec.blockModes = blockModes;
			return this;
		}

		public Builder setEncryptionPaddings(String[] encryptionPaddings) {
			spec.encryptionPaddings = encryptionPaddings;
			return this;
		}

		/* Android API 23+ callers use this while constructing the Roblox
		 * keystore request.  Keep the value in the façade so the call has the
		 * real Android method descriptor instead of throwing NoSuchMethodError. */
		public Builder setDigests(String[] digests) {
			spec.digests = digests;
			return this;
		}

		public Builder setAttestationChallenge(byte[] attestationChallenge) {
			spec.attestationChallenge = attestationChallenge;
			return this;
		}

		public Builder setKeyValidityStart(Date keyValidityStart) {
			spec.keyValidityStart = keyValidityStart;
			return this;
		}

		public Builder setIsStrongBoxBacked(boolean isStrongBoxBacked) {
			spec.isStrongBoxBacked = isStrongBoxBacked;
			return this;
		}

		public Builder setUserAuthenticationRequired(boolean userAuthenticationRequired) {
			spec.userAuthenticationRequired = userAuthenticationRequired;
			return this;
		}

		public Builder setAlgorithmParameterSpec(AlgorithmParameterSpec algorithmParameterSpec) {
			spec.algorithmParameterSpec = algorithmParameterSpec;
			return this;
		}

		public KeyGenParameterSpec build() {
			return spec;
		}
	}

	public int getKeySize() {
		return keySize;
	}

	public String[] getBlockModes() {
		return blockModes;
	}

	public int getPurposes() {
		return purposes;
	}

	public String[] getEncryptionPaddings() {
		return encryptionPaddings;
	}

	public String[] getDigests() {
		return digests;
	}

	public byte[] getAttestationChallenge() {
		return attestationChallenge;
	}

	public Date getKeyValidityStart() {
		return keyValidityStart;
	}

	public boolean isStrongBoxBacked() {
		return isStrongBoxBacked;
	}

	public boolean isUserAuthenticationRequired() {
		return userAuthenticationRequired;
	}

	public String getKeystoreAlias() {
		return keystoreAlias;
	}
}
