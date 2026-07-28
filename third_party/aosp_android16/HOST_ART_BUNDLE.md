# Nuah host ART bundle policy

Nuah consumes a prebuilt host runtime. Normal builds must never synchronize or
compile the AOSP tree.

## Accepted source

The source must provide one Linux x86_64 bundle containing `libart.so`,
`libnativebridge.so`, `dalvikvm64`, the matching Android 15/16/17 boot JARs,
the complete ELF dependency closure, and the host-compatible `libandroidfw`.
The provider must publish a revision, build IDs, checksums, license, and the
glibc baseline.

Priority is:

1. A permitted Sober/Vinegar host-runtime export.
2. A reproducible third-party host-ART release with the same contract.
3. One explicitly maintained GitHub Actions bootstrap artifact, generated
   only when the Android revision changes.

Android device libraries, APEX files, SDK/NDK packages, and the deleted legacy
`art_standalone` tree are not valid sources.

## Bundle contract

`manifest.json` records the Android release, AOSP `art`/`libcore` revisions,
host ABI, glibc baseline, every file checksum, and the bundle format version.
Nuah rejects a bundle with a mismatched architecture, build ID, boot-classpath
checksum, or unresolved `DT_NEEDED` dependency.

The runtime archive is separate from ATL/Nuah binaries and debug symbols.
