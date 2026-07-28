# ATL-Android16 source base

This directory pins the Android 16 release sources selected for the parallel
ATL runtime migration:

| Component | Revision |
| --- | --- |
| `art` | `ed6c006bd06ae060bd9698fd2cb25c4865512ec3` |
| `libcore` | `fff4fcc0cf7f080cf251c1bb57561482b13b218f` |
| `dalvik` (DEX tooling only) | `0c8569a6f7492b1ba639d33e22fe9cf6f45a80ac` |

These revisions are from the AOSP `android16-release` branches. The legacy
`third_party/art_standalone` tree is retained only for transitional host
`androidfw` support; it is no longer an allowed ATL runtime. Android 16
ART must be built with its matching libcore/boot classpath and then receive
the ATL host-Linux adaptations; the Dalvik repository is not a VM in this
release and is retained only for DEX tooling and metadata.

The CI build uses AOSP's `master-art` manifest because that is the supported
unbundled ART build surface. The release revisions above remain the migration
compatibility pins and must be checked whenever the `master-art` build moves
to a new Android release.

## Host integration contract

Android 16's supported output is the `com.android.art` APEX. An APEX is not
itself a drop-in replacement for Nuah's Linux `art-standalone` pkg-config
dependency. CI therefore publishes both the APEX and the host ART pieces
(`dalvikvm64`, `libart`, and `libnativebridge`) while the ATL adapter is being
ported. Nuah now rejects the legacy runtime and requires
`NUAH_ATL_RUNTIME=android16` (explicitly or through bundle discovery); the
host ABI, boot class path, JNI shims, and graphics/input tests are migration
gates rather than a baseline fallback.
