# ATL-Android16 source base

This directory pins the Android 16 release sources selected for the parallel
ATL runtime migration:

| Component | Revision |
| --- | --- |
| `art` | `ed6c006bd06ae060bd9698fd2cb25c4865512ec3` |
| `libcore` | `fff4fcc0cf7f080cf251c1bb57561482b13b218f` |
| `dalvik` (DEX tooling only) | `0c8569a6f7492b1ba639d33e22fe9cf6f45a80ac` |

These revisions are from the AOSP `android16-release` branches. No legacy ART
tree is permitted in Nuah. Android 16 ART must use its matching libcore/boot
classpath and ATL host-Linux adaptations; the Dalvik repository is not a VM in
this release and is retained only for DEX tooling and metadata.

The release revisions above identify the compatibility contract for a prebuilt
host-ART bundle. Nuah downloads and verifies that bundle; it does not compile
or vendor a second ART implementation.

## Host integration contract

Android 16's supported output is the `com.android.art` APEX. An APEX is not
itself a drop-in replacement for Nuah's Linux host runtime. The Nuah bundle
must provide the host pieces (`dalvikvm64`, `libart`, `libnativebridge`,
matching boot jars, dependencies, and `libandroidfw`) in one ABI-checked
artifact. Nuah rejects legacy runtimes and requires
`NUAH_ATL_RUNTIME=android16` (explicitly or through bundle discovery); the
host ABI, boot class path, JNI shims, and graphics/input tests are migration
gates rather than a baseline fallback.
