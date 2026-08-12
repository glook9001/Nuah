package android.security.keystore;

import java.security.InvalidAlgorithmParameterException;
import java.security.KeyPair;
import java.security.SecureRandom;
import java.security.spec.AlgorithmParameterSpec;
import java.security.spec.ECGenParameterSpec;

/**
 * Host-backed replacement for AndroidKeyStore's EC generator.
 *
 * Nuah has no secure hardware keystore.  The Roblox client only needs a
 * normal P-256 key while probing its quote/attestation path, so delegate the
 * actual key generation to the already-installed host provider and retain
 * the pair under the Android alias for the matching KeyStore lookup.
 */
public abstract class KeyPairGenerator extends java.security.KeyPairGeneratorSpi {

	protected java.security.KeyPairGenerator delegate;
	protected AlgorithmParameterSpec params;

	public static final class EC extends KeyPairGenerator {
		public EC() {
			try {
				delegate = java.security.KeyPairGenerator.getInstance("EC", "BC");
			} catch (Exception ignored) {
				try {
					delegate = java.security.KeyPairGenerator.getInstance("EC");
				} catch (Exception failure) {
					throw new java.security.ProviderException(failure);
				}
			}
		}

		@Override
		public void initialize(int keysize, SecureRandom random) {
			delegate.initialize(keysize, random);
		}

		@Override
		public void initialize(AlgorithmParameterSpec parameters,
		                        SecureRandom random)
				throws InvalidAlgorithmParameterException {
			params = parameters;
			if (parameters instanceof KeyGenParameterSpec) {
				/* The Android spec describes purposes and digests in addition to
				 * the key size.  The host EC provider only needs the curve. */
				delegate.initialize(new ECGenParameterSpec("secp256r1"), random);
				return;
			}
			delegate.initialize(parameters, random);
		}

		@Override
		public KeyPair generateKeyPair() {
			final KeyPair pair = delegate.generateKeyPair();
			if (params instanceof KeyGenParameterSpec) {
				final String alias = ((KeyGenParameterSpec) params).getKeystoreAlias();
				if (alias != null && !alias.isEmpty())
					AndroidKeyStore.map.put(alias, pair.getPrivate());
			}
			return pair;
		}
	}
}
