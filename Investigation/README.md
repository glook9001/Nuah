# Sober ELF investigation

Date: 2026-07-26

## Scope

This is a static, black-box investigation of the locally installed Sober Flatpak:

- App ID: `org.vinegarhq.Sober`
- Version: `1.7.1`
- Architecture: `x86_64`
- Flatpak commit: `3f0141ef9c95ff47a08a1437d5b328bd8a6cdf749de592b06579dd5f3fcf948b`
- Sample binaries: `/var/lib/flatpak/app/org.vinegarhq.Sober/x86_64/stable/<commit>/files/bin/`

No Sober binaries were copied into this repository. The repository contains observations and commands only.

## Recording a full JNI contract

Normal Sober clients are ptrace-owned by their controller, so
`live_jni_contract_bcc.py` is the read-only recorder for a normal Roblox
session. It uses PID-filtered BCC uprobes to record `FindClass`, method/field
lookup, and `RegisterNatives` calls without changing Sober or Roblox memory.
Run it while a Roblox room is already running, exercise the intended MVP path
(join, keyboard/mouse, menu, leave), then interrupt it. Convert its raw output
with `normalize_sober_jni_contract.py`; unresolved class pointers remain `*`
and therefore cannot be mistaken for implemented Nuah façade APIs. The older
GDB recorder is retained only for a controller-owned debugging launch.

## Executive findings

1. `sober` and `sober_services` are Linux ELF64 x86-64 PIE executables.
2. Both have no section-header table (`ShOff = 0`, `ShNum = 0`) and are stripped. This is valid for execution but removes much of the metadata normally used by static analysis.
3. `sober` is the larger native runtime. It dynamically loads graphics, audio/video, networking, crypto, fonts, D-Bus, `libloader.so`, and a bundled mimalloc allocator.
4. `sober_services` is a smaller native UI/services process. Its dependencies are GTK 4, libadwaita, WebKitGTK 6, JavaScriptCoreGTK, libsoup, GLib, and GIO.
5. `sober_services` contains direct strings for Android application bundles, APK/XAPK/APKM handling, Roblox IPC, `RobloxWKHybrid`, onboarding UI, and configuration switching.
6. Rust is present in the package, but the evidence is strongest for `libbadcpu.so` and `libloader.so`, not for the main `sober` executable. `sober_services` has C++ ABI evidence.

## ELF header evidence

### `sober`

```text
Type        0x0003          ET_DYN / position-independent image
Machine     0x003e          x86-64
Entrypoint  0x001e5e80
PhOff       0x00000040
ShOff       0x00000000       no section table
PhNum       17
ShNum       0
```

Rizin also reports:

```text
ELF64, little-endian, x86-64
PIE true
stripped true
NX true
RELRO full
interpreter /lib64/ld-linux-x86-64.so.2
rpath $ORIGIN/subprojects/mimalloc:$ORIGIN
```

### `sober_services`

```text
Type        0x0003          ET_DYN / position-independent image
Machine     0x003e          x86-64
Entrypoint  0x0003b940
PhOff       0x00000040
ShOff       0x00000000       no section table
PhNum       13
ShNum       0
```

Rizin reports it as stripped, PIE, dynamically linked through `/lib64/ld-linux-x86-64.so.2`, with NX enabled and no RPATH.

### Interpretation

`ET_DYN` does not mean these are ordinary shared libraries. For a modern PIE executable, Linux loads the image at a randomized address and uses its entry point after the C runtime and dynamic linker initialize it.

The program-header table is retained because the Linux loader needs it. The section-header table is absent because it is primarily useful to linkers and analysis tools. Therefore, Rizin can still identify the entry point, imports, dependencies, and strings, but function names and source-level structure are limited.

## `sober` dependency map

Rizin reports these dynamic libraries:

```text
libloader.so
libmimalloc.so
libcrypto.so.3
libcurl.so.4
libfreetype.so.6
libfontconfig.so.1
libz.so.1
libsecret-1.so.0
libgobject-2.0.so.0
libglib-2.0.so.0
libxml2.so.16
libGLESv2.so.2
libEGL.so.1
libgstreamer-1.0.so.0
libgstapp-1.0.so.0
libgstvideo-1.0.so.0
libdbus-1.so.3
libm.so.6
libgcc_s.so.1
libc.so.6
ld-linux-x86-64.so.2
```

This is consistent with a native runtime that needs:

- EGL/GLES and Vulkan/Wayland/X11 integration for rendering;
- GStreamer app/video interfaces for media;
- curl, OpenSSL, zlib, and XML for network/update/data paths;
- Fontconfig, FreeType, and GLib ecosystem services;
- D-Bus and libsecret integration;
- a private loader and allocator.

The RPATH makes the executable search beside itself for private components:

```text
$ORIGIN/subprojects/mimalloc:$ORIGIN
```

## `sober_services` dependency map

Rizin reports:

```text
libwebkitgtk-6.0.so.4
libgtk-4.so.1
libsoup-3.0.so.0
libglib-2.0.so.0
libgio-2.0.so.0
libjavascriptcoregtk-6.0.so.1
libgobject-2.0.so.0
libadwaita-1.so.0
libgcc_s.so.1
libc.so.6
ld-linux-x86-64.so.2
```

The recovered strings include:

```text
/org/vinegarhq/Sober/onboarding.ui
org.vinegarhq.Sober
Android application bundle files
apks
xapk
apkm
executeRoblox
script-message-received::executeRoblox
RobloxWKHybrid
script-message-received::RobloxWKHybrid
config-switch-
Execute Roblox command is bigger than our IPC buffer! Please report this!
```

These are direct evidence that `sober_services` includes the onboarding/settings/web UI and a bridge for Roblox-related commands. They do not, by themselves, prove the exact IPC protocol or command semantics.

Rizin's analysis recovered a function named `main` at `0x000fafd0` in `sober_services`. The main `sober` executable exposed only `entry0` at `0x001e5e80` in the stripped sample.

## Language/compiler evidence

The language field reported by Rizin is heuristic and says `c` for the two main executables. It does not prove the source language.

Separate package components provide stronger evidence:

### `libbadcpu.so`

Rizin strings include Rust runtime markers:

```text
RUST_BACKTRACE
__rust_begin_short_backtrace
fatal runtime error: Rust panics must be rethrown, aborting
Result::unwrap()
thread panicked while processing panic
```

This is strong evidence that `libbadcpu.so` contains Rust-compiled code or a statically linked Rust runtime.

Its role is also recoverable independently of the language markers.  Rizin finds the `sigaction` import and the diagnostic sequence “CPU does not support SSE 4.2 - will attempt to emulate SSE 4.2 instructions” followed by “error setting SIGILL handler.”  Thus `libbadcpu.so` is a Rust-backed x86-64 CPU-feature guard with a SIGILL-based fallback/emulation path for absent SSE 4.2; it is not evidence for Android signal emulation or the Android library namespace.

### `libloader.so`

Rizin strings include:

```text
RUST_LIB_BACKTRACE
RUST_BACKTRACE
unsupported backtrace
disabled backtrace
```

This is evidence of Rust runtime code in the loader component, although the exact proportion of Rust code cannot be determined from strings alone.

### `sober_services`

The binary contains many C++ ABI names such as:

```text
__cxx11
```

This is strong evidence that C++ code or C++ runtime support is present in `sober_services`.

### Safe conclusion

```text
Sober is mixed-language.
Rust:    strongly evidenced in libbadcpu.so and libloader.so
C++:     strongly evidenced in sober_services and bundled support code
sober:   source language not determinable from this stripped sample
```

## Runtime/crash evidence

An existing syscall trace in `/tmp/sober-memfd.trace` records Flatpak/bwrap setup followed by graphics-related activity and:

```text
SIGSEGV
si_addr=0xfffffffffffffffe
```

The trace shows `memfd_create` calls for Wayland/Mesa-related objects such as `lp_dma_buf`, `allocation fd`, `state table`, and `gdk-wayland` before the crash. This suggests a failure during or shortly after graphics initialization, but the trace alone cannot identify the crashing function.

A new `flatpak run org.vinegarhq.Sober` attempt could not start because the surrounding execution environment returned:

```text
error: Unable to allocate instance id
```

Consequently, the crash has not yet been reproduced under a debugger in this environment.

## Reproducible Rizin commands

Set the sample directory first:

```bash
BASE=/var/lib/flatpak/app/org.vinegarhq.Sober/x86_64/stable/3f0141ef9c95ff47a08a1437d5b328bd8a6cdf749de592b06579dd5f3fcf948b/files/bin
```

Inspect the ELF header:

```bash
rizin -q -c 'iH' "$BASE/sober"
rizin -q -c 'iH' "$BASE/sober_services"
```

Inspect loader metadata and libraries:

```bash
rizin -q -c 'iI;il' "$BASE/sober"
rizin -q -c 'iI;il' "$BASE/sober_services"
```

Search strings:

```bash
rizin -q -c 'izz~Roblox;izz~Android;izz~APK;izz~IPC;izz~config' "$BASE/sober_services"
```

Run analysis and list recovered functions:

```bash
rizin -q -e anal.timeout=20 -c 'aaa;afl' "$BASE/sober_services"
```

## Next analysis steps

The next high-value step is dynamic tracing with a working Flatpak instance, followed by a core dump or debugger session. Static Rizin work is limited by the removed section table and symbols. The safest useful targets are:

1. map `sober_services` `main` and its WebKit/GTK callbacks;
2. identify the IPC read/write boundary around `executeRoblox`;
3. trace the loader's `dlopen` targets and child-process creation;
4. reproduce the graphics crash with `gdb` or a core dump and map the faulting address using the exact build-ID sample.

## Focused runtime findings

### `executeRoblox` and `RobloxWKHybrid`

Rizin found all four relevant strings referenced from one recovered function, `fcn.000fda80`:

```text
0xfde65  script-message-received::executeRoblox
0xfde84  script-message-received::RobloxWKHybrid
0xfdebd  executeRoblox
0xfdecf  RobloxWKHybrid
```

Both signal names are passed the same callback address, `0xfeac0`. This is stronger than merely finding the strings: both WebKit message channels appear to be registered to the same native handler.

The previously unknown **outgoing JSON schema** is recoverable from the cached Roblox Hybrid JavaScript that this WebKit profile loaded.  Android bridge calls construct and pass the following object to `window.__globalRobloxAndroidBridge__.executeRoblox(JSON.stringify(query))`:

```json
{
  "moduleID": "Game",
  "functionName": "launchGame",
  "params": { "request": { "...": "web-supplied launch fields" } },
  "callbackID": "UUID-or-null"
}
```

The four top-level field names are exact.  `moduleID` and `functionName` are generated from the Hybrid module and exported method name; `params` maps formal argument names to values; `callbackID` is a generated UUID when the web caller supplied a callback and `null` otherwise.  The cached `autoGenerateNative` factory is now recovered in full: it places every actual argument under its formal name in `params`, detects function-valued arguments as the callback, then calls `JSON.stringify(query)`.  Consequently a callback passed to `Game.launchGame(request, callback)` is represented only by `callbackID`: the transient `params.callback` function property is omitted by JSON serialization.  The exact serialized shape is therefore `{"moduleID":"Game","functionName":"launchGame","params":{"request":<web request object>},"callbackID":<UUID-or-null>}`.  Likewise, `Game.startWithPlaceID(placeID, callback)` serializes `params` as `{"placeID":<place ID>}`.  It also declares Android launch-mode values `RequestGame`, `RequestFollowUser`, `RequestPrivateGame`, `RequestGameJob`, and `RequestPlayWithParty`; these are application-level request values, not process command-line arguments.

The request object is no longer wholly unknown.  A second cached, actually-loaded Roblox web-application bundle exports `buildPlayGameProperties(rootPlaceId, placeId, gameInstanceId, playerId, privateServerLinkCode, referredByPlayerId, joinData)` and returns exactly:

```json
{
  "rootPlaceId": "…",
  "placeId": "…",
  "gameInstanceId": "…",
  "playerId": "…",
  "privateServerLinkCode": "…",
  "referredByPlayerId": "…",
  "joinData": "…"
}
```

Fields whose arguments are absent remain JavaScript `undefined` and are omitted by JSON serialization.  The bundle's play dispatcher provides the exact selection rules: `placeId == rootPlaceId` plus `gameInstanceId` selects instance join; `rootPlaceId` plus `playerId` selects follow-user; `privateServerLinkCode` selects private-server join; otherwise it selects ordinary multiplayer join.  `joinData` and `referredByPlayerId` are passed on the normal multiplayer path, and the browser adds a generated `joinAttemptId` to its telemetry when enabled.  The legacy in-app wrapper passes this same object to `window.Roblox.Hybrid.Game.launchGame(request, callback)`.  Therefore these seven names are the recovered canonical shape of `params.request`; a message-bearing Sober capture is still needed to show which optional fields a particular detached launch serializes and the central process's final conversion to a Roblox runtime command.

An isolated live helper/socket-pair test exercised the complete bridge with a synthetic, non-dispatched launch object.  It emitted opcode `10` and this compact JSON at packet offset 8:

```json
{"moduleID":"Game","callbackID":null,"params":{"request":{"joinData":"test","rootPlaceId":123,"placeId":123}},"functionName":"launchGame"}
```

The JavaScript source inserted keys in a different order, so this test additionally proves that `jsc_value_to_json` may reorder object properties.  Field names and values—not byte order—are the protocol contract.  The isolated socket had no central Sober reader, so this validates helper serialization without issuing a game-launch request.

The same cached library defines the `RobloxWKHybrid` (iOS/WebKit) envelope as `{"webViewId":...,"command":JSON.stringify(query),"requestId":...}`, where `query` uses the same four-field format.  On Sober's Android branch, no such envelope is added: the four-field object reaches the shared native callback and is serialized unchanged.  The native callback then places it in the payload of a fixed-size IPC packet described below.  This proves the **transport grammar**, but it does not prove which optional fields appear inside a particular live `request` object or how the central Sober reader validates and dispatches it.

The callback at `0xfeac0` has the following statically recoverable behavior:

1. It receives a WebKit message value and obtains a string-like payload through an indirect imported call.
2. It obtains the payload length and rejects/logs a message at or above `0x2001` bytes.
3. It copies at most `0x2000` bytes into a stack buffer.
4. It reserves a `0x2838`-byte stack buffer, writes opcode `0x0a` at byte 0, and copies the compact JSON at byte 8.  It does not initialize the intervening bytes or the unused tail before writing the full record.
5. It writes the first `0x2830` bytes of that packet to a file descriptor held in a global object.
6. The write loop retries when the error value is `4` (`EINTR`).

The important result is:

```text
JavaScript/WebKit message
        → native callback 0xfeac0
        → opcode 0x0a + seven unspecified bytes + compact JSON + unspecified tail
        → one 0x2830-byte file-descriptor IPC record
```

The exact file descriptor endpoint and command grammar are not statically named in this binary. The callback is therefore best described as an IPC command forwarder, not as the Roblox launcher itself.

The central decrypted runtime now supplies the helper-spawn side of that channel.  A function at image address `0x55e800` constructs the literal `sober_services`, resolves the service executable path (the live helper is `/app/bin/sober_services`), formats the socket descriptor in decimal, selects `attached` or `detached` from a mode flag, and builds the exact argv vector:

```text
[
  "/app/bin/sober_services",
  "--server",
  "<decimal Unix-stream-FD>",
  "attached" | "detached",
  NULL
]
```

The resolved call slot at image address `0x738ed8` points to glibc `posix_spawn` (Flatpak runtime libc file offset `0x105bd0`), not `execvp`.  Register setup follows the `posix_spawn(pid, path, file_actions, attributes, argv, envp)` ABI; its `envp` comes through an indirect runtime environment pointer.  The bootstrap's environment sanitization remains in force, so this establishes inherited/sanitized environment passing but not the complete values.  This directly proves that `sober_services` is launched by the central Sober runtime as a helper and not as the Roblox game process.

That reader thread is now recovered too.  Immediately after the spawn setup, the runtime can schedule the closure at image address `0x560100` with the service context.  It owns the parent-side descriptor stored in that context, performs a one-byte `recv(..., MSG_DONTWAIT | MSG_PEEK)` readiness check, accumulates exact `0x2830`-byte records with retry-on-`EINTR` behavior, and dispatches record opcodes including `9`, `10`, `0x11`, and `0x0c`–`0x0f`.  Its opcode-10 branch copies the bytes beginning at packet offset 8 and parses them as JSON.  A complete Rizin control-flow pass now narrows its role further: every recovered JSON-key branch is for configuration/UI state (`rval`, `API_SWITCH`, SDL controller mappings, OpenGL/Wayland and joystick options); there is no `Game`, `launchGame`, or request-field branch in this closure.  Crucially, the constructor at `0x55e800` installs this task only when its caller control byte is zero and the service context has both byte `+0x38 == 0` and byte `+0x02 == 0`; it is not a universal `sober_services` reader.  The observed attached configuration helper takes that path.  A whole-image scan for the fixed `0x2830` record size found no second reader in the decrypted outer runtime: its code references are confined to service setup and this closure.  Therefore the detached `Game.launchGame` consumer is not merely an unrecognized branch of the on-disk/self-remapped outer image; it must be reached through dynamically unpacked code or reside in another process/image.  The detached mode's callback path remains to be located or captured.

`RobloxWKHybrid` is now identifiable from the cached, actually-loaded Roblox Hybrid bundle.  It is the **iOS** WebKit message-handler namespace, not an extra Sober-native command family.  Its `Bridge/iOS` implementation sends `window.webkit.messageHandlers.RobloxWKHybrid.postMessage({webViewId, command: JSON.stringify(query), requestId})`, where `webViewId` comes from the `Hybrid(...)` user-agent token or a generated UUID.  The Android sibling instead sends the same four-field query (`moduleID`, `functionName`, `params`, `callbackID`) as `JSON.stringify(query)` to `window.__globalRobloxAndroidBridge__.executeRoblox`; that is the callback Sober exposes.  The bundle declares Hybrid modules `Game`, `Social`, `Chat`, `Input`, `RealTime`, `Navigation`, `Push`, `Overlay`, and `Localization`, plus the generic callback/event infrastructure.  Its declared methods include, for example, `Game.launchGame`/`startWithPlaceID`, chat/navigation operations, realtime subscribe/unsubscribe, overlay close/submit state, and push/localization triggers.  This is the complete recovered **web API declaration surface**, not proof that Sober implements every declared module/method.  The callback/dispatch path for Android `Game.launchGame` remains separate and unlocated.

The bundle's exact `autoGenerateNative` implementation supplies the command-schema rule for that surface.  It derives names from each JavaScript function's formal parameters, puts every actual argument into `params` under that name, and registers the **last** function-valued argument as the callback.  `JSON.stringify` subsequently omits function-valued `params` properties.  Thus the non-callback JSON parameter schemas declared by the bundle are: `Social.presentShareDialog` `{text,link,imageURL}`, `Social.login` `{options}` (its `errorCallback`, if supplied after `successCallback`, is the one retained as `callbackID`), `Social.logout` `{}`; `Game.launchGame` `{request}`, `Game.startWithPlaceID` `{placeID}`; `Chat.newMessageNotification` `{numUnreadMessages}`, `getTopBarHeight` `{}`, `getKeyboardHeight` `{}`, `enterConversation` `{}`, `leaveConversation` `{}`, `startChatConversation` `{params}`, and `shareGameToChat` `{placeID}`; `RealTime.isConnected` `{}`, `subscribeTopic` `{token,replaceToken}`, and `unsubscribeTopic` `{token}`; `Navigation.navigateToFeature` `{params}`, `openUserProfile` `{userId}`, and `startWebChatConversation` `{userId}`; `Push.pushPermissionTrigger` `{pushPermissionContext}`; `Localization.languageChangeTrigger` `{newRobloxLanguageValue}`; and `Overlay.close` `{}` plus `setSubmitState` `{submitButtonState}`.  This is exact browser-side serialization grammar, not a claim that each command is accepted by Sober's native detached dispatcher.

The same loaded bundle recovers the browser-side **return** ABI.  Its shared `Bridge/Native` module keeps a callback dictionary indexed by the generated UUID and implements `nativeCallback(callbackID, status, params)`: if the ID is present, it deletes that entry and invokes the stored JavaScript callback as `callback(status, params)`.  `Bridge.CallbackStatus` fixes the only two declared status values as `SUCCESS: 0` and `FAILURE: 1`.  Android's platform module supplies only the outbound `execute` override, so the shared `nativeCallback` remains the callback ingress visible to Android Hybrid pages.  This establishes the exact page-level result shape, but not the native source of `status`/`params`.  The already-recovered central-to-helper opcode for evaluating JavaScript is a plausible delivery mechanism for a call such as `window.Roblox.Hybrid.Bridge.nativeCallback(...)`; that linkage remains an inference until a live callback is observed.

### Bidirectional `--server` socket protocol

ReOxide/Ghidra decompilation of the helper's socket loop (Rizin `0xfb720`, Ghidra `0x1fb720`) establishes that the passed `--server` Unix socket is bidirectional.  The helper is a receiver for a distinct outer-to-helper control protocol as well as a sender of fixed-size WebKit JSON packets.  It uses `recv()` with `MSG_DONTWAIT | MSG_PEEK` readiness guards, then consumes one exact **`0x2830`-byte (10,288-byte)** control record.  Byte 0 of that record is an opcode; incomplete records are accumulated by repeated `recv` calls.

Confirmed opcode actions include:

| Opcode | Helper action |
| --- | --- |
| `0` | `webkit_web_view_load_uri(web_view, payload + 1)` |
| `2` | `gtk_window_set_title(window, payload + 1)` |
| `3` | `gtk_widget_set_visible(window, payload[1])` |
| `4` | `webkit_web_view_evaluate_javascript(web_view, payload + 1, ...)` |
| `5` | `g_application_quit()` |

The receiver also contains UI/configuration opcodes `1`, `6`, `7`, `8`, and `0x11`; these operate on cookies, onboarding/navigation pages, dialogs, and settings state, but their full payload schemas have not yet been assigned.  In attached mode it performs an additional one-byte nonblocking peek before the normal record loop.  This is a readiness/handshake distinction, not an observed JSON format.

This must not be conflated with the opposite direction: WebKit `executeRoblox` and `RobloxWKHybrid` callbacks serialize their JavaScript value to compact JSON, then send one `0x2830`-byte opcode-10 packet: byte 0 is `0x0a` and JSON starts at byte 8.  Bytes 1--7 and the tail after JSON are not initialized by the recovered callback and must be treated as unspecified, not as protocol data.  Thus both directions share the record size but use their own opcode payloads.  The attached-settings central reader's opcode-10 branch is the observed JSON consumer; the command-specific detached-game dispatcher remains unassigned.

The fixed record size has independent live validation.  During `sober config`, `ss -xap` showed the helper endpoint's receive queue at `20576` bytes while it held `--server 21`; `20576` is exactly `2 × 0x2830`.  The peer endpoint had a different socket inode, as expected for `socketpair`, but its owning process was not disclosed and its protected `/proc/<outer>/fd` table could not be enumerated.  This proves that the outer side was actively sending two complete helper-control records, while leaving the peer process identity and their opcode payloads unobserved.

The peer process was subsequently identified without consuming any socket traffic.  `pidfd_getfd` was used solely to duplicate candidate descriptors and `fstat` their inode numbers; a bounded scan of the live config-mode Sober tree found the socket-pair peer at FD `20` in the helper's direct parent, the central `sober` process (PID `98098` in that run).  The helper held the other endpoint at FD `21`.  Thus the concrete reader for messages emitted by the **attached settings helper** is its parent `sober` process, not `sober_services` and not an external daemon.  FD numbers are per-run and must not be treated as constants.  Static analysis now identifies the parser as the central opcode-10 record handler; a message-bearing detached-mode capture is still needed to assign `Game.launchGame` dispatch.

### Recovered `sober_services --server` transport

ReOxide-backed Ghidra decompilation of `sober_services`' `main` (Rizin `0xfafd0`, Ghidra `0x1fafd0`) establishes the helper's exact server-mode invocation:

```text
sober_services --server <decimal-file-descriptor> [detached]
```

`main` accepts this mode only when there are at least four argv entries, parses `argv[2]` with base 10, stores the resulting integer in the global later read by the WebKit callback, and treats `argv[3] == "detached"` as a boolean flag.  It then creates/activates a D-Bus application using the ID `org.vinegarhq.Sober`.  The callback at Rizin `0xfeac0` calls `jsc_value_to_json(value, 0)`, rejects serialized messages larger than `0x2000` bytes, reserves a `0x2838`-byte stack packet, writes opcode `0x0a` at offset 0, copies compact JSON to offset 8, and writes exactly `0x2830` bytes to that stored descriptor, retrying an interrupted write.  The remaining bytes are not initialized first.  The imported-function identification comes directly from the dynamic relocation entry at raw GOT address `0x115ae8`; the adjacent entries resolve to `strncpy`, `strlen`, and `write`.

Therefore the exact helper-to-parent framing is:

```text
byte 0:       0x0a
bytes 1--7:    unspecified stack bytes (not protocol fields)
bytes 8--...:  compact JSON, followed by unspecified stack bytes
total:         0x2830 bytes
```

The helper contains two encrypted static blobs which are decoded at startup and passed to WebKit.  Decoding the 362-byte user script proves the `executeRoblox` payload is JSON:

```javascript
window.__globalRobloxAndroidBridge__ = {
  executeRoblox: (query) => {
    const json = JSON.parse(query);
    console.info("__globalRobloxAndroidBridge__: executeRoblox: ", json);
    window.webkit.messageHandlers.executeRoblox.postMessage(json);
  },
};
```

So the complete proven flow is:

```text
page supplies JSON text
  → __globalRobloxAndroidBridge__.executeRoblox JSON.parse()
  → WebKit postMessage(JSON value)
  → native callback jsc_value_to_json(..., 0)
  → opcode-10 0x2830-byte packet to --server FD
```

The second decoded blob is the WebKit user-agent string, identifying itself as a Roblox Android tablet hybrid application (`ROBLOX Android App 2.730.790 ... RobloxApp/2.730.790(GlobalDist; GooglePlayStore)`).  There is still no Roblox process creation in the callback.  `sober_services` is a WebKit/D-Bus bridge which delegates its packet to the central `sober` process through the passed FD.  The generic outgoing JSON envelope is recovered and its central parser is identified, but a concrete live `params.request` payload and detached-game dispatch interpretation remain uncaptured.

The helper's dynamic symbol table further supports that boundary: it has no imported `exec*`, `fork`, `vfork`, `posix_spawn*`, `system`, `popen`, `clone`, `socketpair`, `pipe2`, `dlopen`, or `dlsym` symbol.  Its declared dependencies are WebKitGTK, GTK4/libadwaita, libsoup, GLib/GIO/GObject, JavaScriptCoreGTK, libgcc, and libc.  This does not rule out an intentionally hidden raw-syscall launcher in principle, but, together with the recovered callback and its sole WebKit/FD transport, it is strong direct evidence that `sober_services` does **not** launch Roblox in the observed service path.

A raw x86-64 instruction pass closes that residual raw-syscall caveat for this helper binary.  All `0f 05` byte-pair hits before the executable `PT_LOAD` are exception-table metadata; the hits in the executable segment cross ordinary branch/NOP/padding instruction bytes and do not decode as `syscall`.  No decoded `syscall` instruction is present in `sober_services`.  Combined with the absent process-creation imports, this rules out a direct raw-syscall launcher in this installed helper, rather than merely making it unlikely.

### `libloader.so` process creation

Rizin identified these call sites in `libloader.so`:

```text
0x16ea97  fork
0x16f0d3  fork fallback/path
0x16f5cd  dup2
0x16f619  dup2
0x16f66d  dup2
0x16f6bd  setgroups
0x16f6dd  setgid
0x16f738  setuid
0x16f750  chroot
0x16f76b  chdir
0x16f78d  setpgid
0x16f7a6  setsid
0x16f898  execvp
```

The child setup function at `0x16f5a0` conditionally:

- duplicates configured file descriptors onto stdin, stdout, and stderr;
- installs supplementary groups;
- drops group and user identity;
- changes root with `chroot` when configured;
- changes working directory;
- creates a process group/session;
- executes a configured program through `execvp`.

The field guards are visible in the Rizin control flow, not inferred from imports.  The descriptor configuration passed in `rsi` independently enables `dup2(fd, 0)`, `dup2(fd, 1)`, and `dup2(fd, 2)`.  The caller-supplied execution structure in `rdi` supplies: a supplementary-group vector/count at `+0x70/+0x78`; optional GID and UID flags/values at `+0xc8/+0xcc` and `+0xd0/+0xd4`; optional `chroot` and working-directory strings at `+0x58` and `+0xb0`; process-group/session flags at `+0xc0/+0xc4` and `+0xa8`; program pathname at `+0x20`; and `argv` at `+0x88`.  If the separate environment argument is non-null, the routine temporarily replaces the global `environ` pointer immediately before `execvp` and restores it if execution fails.  It also clears `SIGPIPE` and invokes caller-provided pre-exec callbacks.  Thus an actual executable, complete argv, and environment can only be learned by recovering a populated instance of this structure; no hard-coded Roblox command is present in this routine.

The parent-side path uses `pipe2`, `recvmsg`, `close`, and `posix_spawn` support.  A caller trace now makes the hierarchy exact: the launch wrapper passes the configuration's program field (`+0x20`) and argv field (`+0x88`) directly to `pidfd_spawnp` when that API is available; otherwise it calls `posix_spawnp` with the same configured program/argv and its prepared file actions, spawn attributes, and environment vector.  The `0x16f5a0` `execvp` setup routine is reached only through the wrapper's fork fallback.  No call site inside `libloader.so` populates a fixed Roblox executable name, argv, or environment—the wrapper is a generic native process-launch abstraction rather than a single hard-coded `exec("sober_services")` call.

The loader also contains a separate function at `0x1352c0` that iterates over library/path records and calls:

```text
dlopen(path, 0x102)
```

The `0x102` flags are consistent with `RTLD_NOW | RTLD_GLOBAL`.  A deeper Rizin pass now recovers the record and result behavior: the input is an ordered slice of 16-byte `(pointer, byte_length)` path descriptors, rather than an array of C strings.  For each descriptor, `0x1352c0` allocates a temporary `length + 1` byte buffer, copies exactly that many bytes, appends a NUL byte, calls `dlopen`, then releases the temporary buffer.  Each non-null returned handle is appended to an output vector in input order; a failed load terminates that pass and leaves only the preceding successful handles.  The directly observable caller (`0x1116a0`) simply forwards such an input slice and returns the handle vector.  Neither function embeds a Roblox, Android-system-library, or shim-library pathname.  Thus this proves a generic ordered `RTLD_NOW | RTLD_GLOBAL` loader primitive, but not the dynamically constructed library-record list used in a normal Roblox launch.

### Android-to-Linux boundary

The installed Sober data directory contains the actual x86-64 Roblox Android package:

```text
packages/x86_64/com.roblox.client/base.apk
packages/x86_64/com.roblox.client/split_config.x86_64.apk
```

The split APK contains these Android-native ELF libraries:

```text
lib/x86_64/libroblox.so
lib/x86_64/libtrampoline.so
lib/x86_64/libbacktrace-native.so
lib/x86_64/libdatastore_shared_counter.so
lib/x86_64/libeigen_blas.so
lib/x86_64/libeigen_lapack.so
lib/x86_64/libimage_processing_util_jni.so
lib/x86_64/libsurface_util_jni.so
lib/x86_64/librenderscript-toolkit.so
lib/x86_64/libyuv_shared.so
lib/x86_64/libzstd-jni-1.5.7-6.so
```

`libroblox.so` is an Android x86-64 ELF built with the Android NDK. Its dependencies include:

```text
libOpenMAXAL.so
libmediandk.so
libOpenSLES.so
libGLESv2.so
libEGL.so
libandroid.so
liblog.so
libm.so
libdl.so
libc.so
```

It also contains Android/JNI entry points such as `Java_com_google_androidgamesdk_GameActivity_initializeNativeCode` and many `Java_com_roblox_universalapp_*` methods.

Its undefined dynamic-symbol table gives the exact Android-facing native API surface requested by the primary client.  It imports asset operations (`AAssetManager_fromJava`, `AAssetManager_open`, `AAsset_getBuffer`, `AAsset_getLength`, and `AAsset_openFileDescriptor`); configuration operations (`AConfiguration_new`, `AConfiguration_fromAssetManager`, language/country/navigation/screen queries, and deletion); looper operations (`ALooper_prepare`, `ALooper_forThread`, `ALooper_addFd`, `ALooper_pollOnce`, and lifetime/removal calls); and native-window conversion/lifetime/dimension operations (`ANativeWindow_fromSurface`, acquire/release, width, and height).  It also imports the full listed NDK `AMediaCodec`/`AMediaFormat` encode/decode queue and buffer API, Android logging (`__android_log_*`), `__system_property_get`, `android_set_abort_message`, and `slCreateEngine`.

For graphics it imports Android `libEGL`'s display/config/context/window-surface/swap API (`eglGetDisplay`, `eglInitialize`, `eglChooseConfig`, `eglCreateContext`, `eglCreateWindowSurface`, `eglMakeCurrent`, `eglSwapBuffers`, and related lifecycle/query functions) plus the GLES 2 rendering API.  This is an import inventory, not a claim that every call is reached in the sampled session.  The live relocation work separately proves only selected direct Android imports—including the asset, configuration, looper, native-window, property/logging, media, OpenSL ES, and Android EGL groups—resolve into Sober's self-remapped `/memfd:sober` namespace rather than pathname-visible Android system libraries.  The function-by-function implementation/semantics for most of those entries remains unobserved.

`libtrampoline.so` is a small Android PIE executable with:

```text
/system/bin/linker64
__libc_init
dlopen
dlsym
dlerror
__android_log_print
```

Its strings identify `crashpad_trampoline` and `CrashpadHandlerMain`.

Rizin disassembly of the extracted `libtrampoline.so` establishes its exact narrow role.  Its Android entry calls `__libc_init`; its application entry requires at least one argument, calls `dlopen(argv[1], 0x101)` (`RTLD_LAZY | RTLD_GLOBAL`), resolves `CrashpadHandlerMain`, then tail-calls that function with `argc - 1` and `argv + 1`.  It reports load/lookup failures through `__android_log_print`.  Therefore `libtrampoline.so` is a generic Android **Crashpad handler** bootstrap, not evidence that it loads `libroblox.so` or implements the Android-to-Linux API compatibility layer.  Its `/system/bin/linker64` interpreter remains an Android ABI expectation which Sober must satisfy for this object to run.

This establishes that Sober is not translating an ARM APK through an emulator in this installation. It uses the x86-64 Android build directly. The compatibility work is instead the native runtime boundary that makes Android-oriented code and libraries work in a Linux process.

A complete installed-Flatpak inventory excludes a hidden packaged Android shim: its only native executable/shared objects are `sober`, `sober_services`, `libloader.so`, `libmimalloc.so`, and `libbadcpu.so`.  The additional objects above live only in the x86-64 APK split and are Roblox payload/support libraries.  Thus the observed Android API providers cannot be a separate on-disk Flatpak compatibility library; this independently agrees with the live relocation evidence that the direct Android imports resolve into the self-remapped `/memfd:sober` runtime.

The complete split-wide native dependency inventory also bounds Android compatibility beyond the main client.  `libbacktrace-native.so` imports Android logging and `__system_property_get`; `libimage_processing_util_jni.so` and `libsurface_util_jni.so` require `libandroid.so` native-window operations; and `libimage_processing_util_jni.so` plus `librenderscript-toolkit.so` require `libjnigraphics.so`'s `AndroidBitmap_getInfo`, `AndroidBitmap_lockPixels`, and `AndroidBitmap_unlockPixels`.  `libtrampoline.so` requires Android logging and `__libc_init`.  The remaining payload support objects (`libdatastore_shared_counter`, Eigen BLAS/LAPACK, libyuv, and zstd JNI) list only the Android C runtime/math/dynamic-loader baseline or another payload library.  This is a **potential payload API surface**, not evidence that every support object is loaded in the observed normal Roblox session; only the direct `libroblox.so` bindings have been resolved live.

The logs corroborate this boundary: Roblox initializes `AndroidGLView`, requests `VK_KHR_android_surface`, and then Sober reports the host Intel GPU and host Vulkan driver. Thus the graphics path is Android-client code using a Linux-provided Vulkan/EGL environment, with Sober adapting the platform-facing layer.

The Vulkan extension boundary can be stated more precisely.  The installed Android `libroblox.so` contains `vkCreateAndroidSurfaceKHR` and Android hardware-buffer surface APIs.  The exact `libvulkan.so.1` from Sober's Flatpak GNOME runtime exports `vkGetInstanceProcAddr`, `vkCreateWaylandSurfaceKHR`, and `vkCreateXlibSurfaceKHR`, but **does not** export `vkCreateAndroidSurfaceKHR`.  Thus the successful live request for `VK_KHR_android_surface` cannot be handled by the host Vulkan loader unchanged.  A Sober-provided Android Vulkan dispatch/shim layer must either supply `vkCreateAndroidSurfaceKHR` through the Android-facing library namespace or translate the Android request before host dispatch, producing a host Wayland/X11 surface backed by Sober's native window.  The selected host backend is Vulkan on this machine; the precise function and owning in-memory object are still unobserved.

A controlled run with `VK_LOADER_DEBUG=all` rules out a loader-manifest implementation of that shim.  The host loader searched the app's config/data Vulkan manifest paths as well as the Flatpak runtime paths, but discovered only the normal Mesa implicit layers and Mesa ICD manifests; it reported no Sober layer, ICD, or Android-surface manifest.  Since the same run successfully enables `VK_KHR_android_surface`, the translation is above the host loader's manifest/layer mechanism—consistent with the decrypted Sober dispatch registry—rather than a configurable Vulkan loader plugin.

The decrypted Sober runtime also contains explicit host-surface records for `vkCreateWaylandSurfaceKHR`, `vkCreateWaylandSurfaceKHR failed: %s`, `vkCreateXlibSurfaceKHR`, `vkCreateXlibSurfaceKHR failed: %s`, and `SDL.window.create.vulkan`.  Thus the packed runtime has compiled support for both host Wayland and Xlib Vulkan surface creation downstream of its Android-facing interface.  This establishes the available translation targets; it does not by itself prove which branch produced a particular live surface.

What remains unproven is the exact implementation of the Android API shim: which symbols are intercepted, which Android libraries are replaced, and whether the adaptations are implemented in `sober`, `libloader.so`, or additional runtime objects loaded dynamically.

## ReOxide status

ReOxide 0.8.0 was installed in an isolated temporary virtual environment. Its daemon successfully loaded the bundled Rust-oriented plugins:

```text
libprintrust.so
libprintmir.so
libcore.so
```

The ReOxide manager reports a `rust-language` pipeline. A temporary copy of the Flatpak Ghidra tree was used so the system Ghidra installation was not modified. After repairing that temporary copy's Flatpak-specific launch-properties symlink and replacing the obsolete headless Jython helper with `Investigation/DecompileAddresses.java`, the ReOxide-backed Ghidra decompiler ran successfully on `libloader.so`.

The independent decompilation of Rizin's child-launch routine (`Rizin 0x16f5a0`, Ghidra `0x26f5a0` because Ghidra rebases the PIE by `0x100000`) confirms the earlier interpretation: three optional `dup2` calls, optional identity/root/directory/session changes, pre-exec callbacks, and cleanup around the final execution path.  It does **not** expose a hard-coded Sober command, which reinforces that this is a general launcher primitive rather than proof that it launches Roblox or `sober_services` in the normal case.  Rizin remains the clearer source for the imported-symbol names; the stripped ELF's malformed/absent hash and relocation metadata leave the Ghidra pseudocode's indirect calls unnamed.

The dynamic-link interface further limits claims about `libloader.so`.  It has no usable exported dynamic symbols for the outer `sober` executable to import.  Its five `DT_INIT_ARRAY` relocations resolve to `0x170ac0`, `0x13e050`, `0x142570`, `0x180b0`, and `0x135220`: Rizin shows argument/TLS bookkeeping, one-time `sysconf` initialization, a runtime flag, callback/drop plumbing, and TLS initialization respectively.  None reaches the recovered `execvp`/spawn routine or the ordered `dlopen` routine.  Thus there is no evidence that merely loading the library constructor-launches Roblox; its generic launcher remains available capability code whose normal caller is still unlocated.

## Runtime launch evidence (2026-07-26)

### What launches what

A syscall trace of the Flatpak launcher reached the native runtime before Sober deliberately refused to continue under an external tracer.  It proves this diagnostic launch chain:

```text
flatpak run org.vinegarhq.Sober
  -> bubblewrap sandbox
  -> /app/bin/sober
  -> /app/bin/sober_services <diagnostic-message>
```

The recorded `execve` was:

```text
execve("/app/bin/sober_services",
       ["/app/bin/sober_services", "Failed to initialize core functionality ... ptrace_scope ..."],
       environment)
```

This proves that the `sober` process invokes `sober_services` in its diagnostic path.  `libloader.so` may still provide the generic `execvp` mechanism used by that process, so this trace does not attribute the call site to one binary at function granularity.  The second argument is specifically the failure message from a tracer-detection path; it must **not** be treated as the normal service argument or the `executeRoblox` command format.

In an untraced launch, the process tree was instead:

```text
Flatpak bubblewrap (sandbox PID 1)
  sober (PID 2)
    sober child / "Main" (PID 3)
      worker processes
```

The `Main` child had `TracerPid` equal to the parent `sober` process.  This is direct runtime evidence that Sober creates and controls a traced child process.  An external `strace` prevents that arrangement, explaining the startup failure under tracing.  The exact ptrace request sequence remains unknown because tracing it changes the behavior being measured.

The main `sober` binary also contains a `ptrace` reference, independently supporting the observed parent/child tracing relationship.

An audit-safe live snapshot makes the process topology more exact: an initial `sober` process starts a `Main` process, which in turn starts two more processes whose command lines are all `/proc/self/exe` and whose `comm` names remain `sober`.  The audit module saw no `execvp`, `execve`, `posix_spawn*`, `socketpair`, `pipe2`, or selected raw-syscall transition while this tree formed, even though the Android client reached its live UI.  Thus this is not evidence for `libloader.so`'s generic `execvp` route as the normal Roblox bootstrap.  The fork/clone operation is either custom code outside the audited PLT/syscall entry points or occurs before/under a mechanism the audit interface cannot interpose; the exact mechanism remains unknown.

A subsequent bounded healthy run sharpens the ownership boundary within that tree.  The traced `Main` process mapped the self-remapped `/memfd:sober` image plus repeated RX/RW views of one deleted memfd (inode `16097` in that run), whose mapped size/segment layout matches the recovered live `libroblox.so`.  Its two later `sober` children inherited only the `/memfd:sober` mappings and had no mapping of that `libroblox.so` memfd.  Therefore the Android payload is owned by `Main`, rather than by those later workers or by `sober_services`.  The run reached `SingleSurfaceApp`'s `LuaApp` stage, emitted `did_handle_app_startup`, and created a Vulkan 1920×1006 swapchain before the imposed 55-second timeout.  This is a mapping/ownership observation, not proof of the custom operation that originally created `Main`.

The outer ELF has a direct dynamic `ptrace` import, and the audit module records its binding, but neither a redirected PLT entry nor a redirected `dlsym("ptrace")` result was called during a successful startup.  This does **not** weaken the independently observed nonzero `TracerPid` of `Main`: it narrows the call site to a custom resolver, direct code in an unpacked/self-remapped image, or another non-interposable route.  It also explains why ordinary `LD_PRELOAD`/`LD_AUDIT` tracing has not exposed the exact ptrace request sequence.

The captured decrypted runtime's executable `PT_LOAD` was also raw-disassembled from its true segment start (`0x1e5e80`) through its full `0x537bb0` bytes.  It contains no decoded x86-64 `syscall` instruction under that linear disassembly; prior raw `0f 05` byte hits were not instruction evidence.  This rules out a straightforward inline-syscall implementation in that particular recovered runtime image.  It does not resolve the trace setup because the required code may use another indirect route or belong to a later-unpacked image absent from the capture.

A separate bounded, uninstrumented normal launch was sampled two seconds after invocation.  At that point the fresh sandbox tree was `bwrap → bwrap → sober (sandbox PID 2) → sober (sandbox PID 3)`.  The PID-3 process had `PPid` and `TracerPid` both set to PID 2; no fresh `sober_services` process existed yet.  This reproduces the parent-traces-child fact without relying on the longer audit run, and bounds it to the earliest healthy startup phase.  It does not show the later worker creation or identify the process-creation/ptrace call site.

A further uninstrumented seven-second snapshot reproduced the topology after the Android client reached a live Vulkan swapchain.  The sandbox parent `sober` had `TracerPid: 0`; its traced game child and both later worker children had command lines consisting of exactly `/proc/self/exe` (one NUL-terminated argument, no trailing launch arguments) and showed the parent as `TracerPid`.  No `sober_services` process was present.  Their initial-stack environment-name sets matched the earlier sanitization result: ordinary Flatpak/session variables plus `LD_BIND_NOW`, but no `LD_PRELOAD`, `LD_AUDIT`, `LD_DEBUG`, or `LD_LIBRARY_PATH`.  This is direct evidence that the normal Roblox bootstrap is a self-reexecution/self-remapped Sober process rather than an `execvp` of a separately named executable; it still cannot reveal the custom creation primitive or value-level environment construction.

An `LD_AUDIT` generation which observed, but did not redirect, additional process APIs kept a normal client startup healthy.  It saw no binding or PLT entry into glibc `clone`, `vfork`, or `pthread_setname_np`.  It did see repeated `prctl(0x53564d41, ...)` entries; `0x53564d41` is `PR_SET_VMA`, used to annotate anonymous virtual-memory areas, not `PR_SET_NAME`.  Therefore the visible `Main` `comm` name is not explained by an interposable `prctl(PR_SET_NAME)`/`pthread_setname_np` call, and the custom parent→child mechanism remains outside these observable APIs.

### Observed Linux isolation

The isolation actually used for this launch came from Flatpak/bubblewrap, not from the optional `chroot` path found in `libloader.so`:

- bubblewrap created new mount, user, and PID namespaces;
- its user namespace mapped sandbox UID/GID `0` to host UID/GID `1000`;
- the sandbox had `NoNewPrivs: 1` and a seccomp filter;
- Flatpak mounted the application/runtime read-only and bound the application's persistent data, cache, config, temporary directory, graphics devices, Wayland socket, audio socket, and D-Bus proxies.

The observed Android-runtime child retained UID/GID 1000 and had the Flatpak seccomp constraints.  No execution of `libloader.so`'s `setuid`, `setgid`, `chroot`, or `setsid` branch was observed in this normal launch.  That code is therefore a capability of the generic loader, not evidence of the normal Sober policy.

Direct parsing of `libloader.so`'s dynamic relocation table independently limits that capability set.  It resolves `fork`, `pipe2`, `socketpair`, `chdir`, `chroot`, `setgroups`, `setgid`, `setuid`, `setpgid`, `setsid`, and `execvp`; it has no dynamically resolved `unshare`, `setns`, `mount`, `umount2`, `clone`, `prctl`, or seccomp API.  This does not rule out raw syscalls absolutely, but together with the observed normal launch it is strong evidence that `libloader`'s normal role is process setup rather than Linux namespace construction.

### Android runtime boundary observed live

The installed APK manifest identifies the Android-side entry chain precisely.  The exported launcher activity is `com.roblox.client.startup.ActivitySplash` (`MAIN` + `LAUNCHER`, `singleTop` launch mode).  It declares the task-retained `singleTask` activity `com.roblox.client.ActivityNativeMain`, alongside the corresponding non-exported, task-retained `singleTask` game activity `com.roblox.client.startup.MainGameActivity`.  The name `ActivityNativeMain` indicates its role but does not, by itself, prove use of Android's `NativeActivity` framework.  `com.roblox.client.ActivityProtocolLaunch` is the exported, no-history `VIEW`/`BROWSABLE` deep-link activity.  The Linux process does not expose Android Activities as separate processes; these manifest roles align with the observed `AndroidGLView`/`JNIAppBridge`/`SingleSurfaceApp` path that Sober supplies inside one Linux runtime.

DEX metadata identifies the JNI handoff inside that chain.  `MainGameActivity` declares the native methods `nativeAppBridgeSetInitParams(InitParams)`, `nativeRetryInit()`, and `nativeSetAssetPath(String)`, while its normal Android lifecycle methods handle creation, resume, configuration changes, new intents, keys, and permission results.  This provides a direct Android-side predecessor for the logged `JNIAppBridge` initialization and asset-folder setup: launcher/deep-link activity state is handed to `MainGameActivity`, which supplies init parameters and paths to the native `libroblox` stack; `SingleSurfaceApp` then initializes the Lua application and rendering surface.

The asset-path handoff is now resolved at the Android boundary.  During `MainGameActivity.onCreate`, `I2()` obtains a string from the APK's XAPK asset manager and passes it through the companion wrapper to `nativeSetAssetPath(String)`.  That manager creates the app-private directory returned by `Context.getDir("assets", 0)`, unpacks/verifies APK assets there, resolves `content` below that directory with `toRealPath()`, and returns its filesystem path.  Its copy check compares the packaged asset stream with the cached file (including size and byte comparison) and rewrites a missing or changed file.  Therefore the native client is passed a real, unpacked **`<Android app-private>/assets/content`** directory rather than an APK/ZIP path.  Repeated normal-start logs make that Android-private root concrete in this Flatpak install: `SingleSurfaceApp` receives `setAssetFolder /home/pepe/.var/app/org.vinegarhq.Sober/data/sober/assets/content/` and `setExtraAssetFolder /home/pepe/.var/app/org.vinegarhq.Sober/data/sober/assets/ExtraContent`.  Thus Sober exposes unpacked assets to `libroblox.so` as ordinary Linux files in the per-app Flatpak data directory.  The internal outer-runtime code that constructs Android's `Context.getDir` result is still unrecovered, but the resulting Linux path and two asset roots are now directly observed.

The bytecode resolves the activity routing rather than leaving it at manifest level: `ActivitySplash.onCreate` constructs an `Intent` for `MainGameActivity`, copies its incoming extras, calls `startActivity`, and immediately finishes.  `ActivityProtocolLaunch.onCreate` parses the browsable deep-link intent and, depending on the route, starts `MainGameActivity` directly or starts `ActivitySplash`; it then finishes.  Thus the normal Android logical sequence is **launcher/deep link → `MainGameActivity` → JNI init/asset path → `libroblox` → `SingleSurfaceApp`/Lua/Vulkan**, all represented by Sober within its Linux runtime process.

The installed Linux desktop entry establishes the preceding host-side handoff: it registers `x-scheme-handler/roblox` and `x-scheme-handler/roblox-player` and executes `sober -- %u`.  Therefore a desktop URI is passed verbatim as one argument after a literal `--` to the outer Sober executable; it is then Sober's responsibility to present the corresponding Android-style intent to `ActivityProtocolLaunch` or another internal route.  The desktop file does not itself translate URI fields or invoke `sober_services`, so it must not be conflated with the detached WebKit `executeRoblox` socket protocol.

Rizin analysis of `classes2.dex` recovers an additional Android-native launch grammar used when constructing a push-notification intent for `ActivitySplash`.  One routine chooses one of these exact URI templates, then calls `Uri.parse`, sets the URI as the `Intent` data, and marks the intent as push-originated:

```text
roblox://placeId=%d&callId=%s
roblox://placeId=%d&gameInstanceId=%s&callId=%s
roblox://placeId=%d&reservedServerAccessCode=%s&callId=%s
```

This confirms native support for normal-place, instance/job, and reserved-server deep-link inputs.  It also exposes a significant naming boundary: the cached WebKit `Game.launchGame` request uses `privateServerLinkCode`, whereas this Android URI uses `reservedServerAccessCode`; `callId` is likewise not one of the seven recovered WebKit request keys.  The static evidence proves both grammars exist, but does not yet prove the exact conversion between them or that the detached Sober dispatcher uses this URI path.

The same DEX contains a more direct Android JSON-launch normalizer.  A synthetic method in `ActivityNativeMain` calls a static handler taking `(JSONObject, Activity)`.  That handler reads these JSON keys with `optLong`/`optString` before building an internal launch-parameter object and passing it, together with the current `Activity`, to the next launch routine:

```text
placeId, userId, conversationId,
gameInstanceId, reservedServerAccessCode, callId,
accessCode, linkCode, launchData,
eventId, gameJoinContext, joinAttemptId, joinAttemptOrigin, isoContext
```

The parser provides empty/zero fallbacks for absent optional values before constructing that object.  This is direct evidence that Android's runtime-side launch model accepts both `gameInstanceId` and `reservedServerAccessCode`, plus telemetry/context fields beyond the WebKit API's seven canonical keys.  It still does **not** prove that Sober maps `privateServerLinkCode` to either `reservedServerAccessCode`, `accessCode`, or `linkCode`, nor that this `ActivityNativeMain` path receives the detached `executeRoblox` socket record; those remain the key unobserved conversion steps.

Following that handler's caller resolves the latter ambiguity.  `ActivityNativeMain` installs the `JSONObject` callback only after `NativeGLInterface.isColdStartDeeplinkToGame()` returns true; the code then registers the callback with an internal asynchronous object and separately serializes the pending deep-link JSON for an activity-side helper.  When the callback fires, it forwards straight into the normalizer above.  Thus this is a **cold-start native deep-link** completion path, not direct evidence that the WebKit `executeRoblox` packet enters `ActivityNativeMain`.  The overlap in field names remains useful evidence of the Android launch model, but the detached WebKit-to-native handoff remains unlocated.

The other static callers do not change that attribution.  The `wk.l0` caller is the callback registered by the same activity-side helper receiving the serialized pending deep-link JSON, and its adapter forwards directly to the normalizer.  A second adapter obtains an Android `Activity` from a fragment/controller and likewise forwards a `JSONObject` to the same normalizer.  Neither registration chain references WebKit, `executeRoblox`, `RobloxWKHybrid`, or the Sober `--server` socket.  They establish reusable Android UI/deep-link entry points, not a recovered detached bridge consumer.

The normalizer's result is the concrete Java/JNI-side `StartGameParams` model, rather than an ad hoc map.  Its generated builder declares `surface`, `platformParams`, `deviceParams`, `placeId`, `userId`, `conversationId`, `gameId`, `username`, `isUnder13`, `joinRequestType`, `referredByPlayerId`, `vrContext`, `accessCode`, `linkCode`, `reservedServerAccessCode`, `callId`, `referralPage`, `launchData`, `gameJoinContext`, `eventId`, `joinAttemptId`, `joinAttemptOrigin`, and `isoContext`.  The builder validates a subset as required before constructing `AutoValue_StartGameParams`; the remaining fields are represented explicitly as optional/defaulted values.  This gives the native Android runtime's actual launch-parameter container and confirms that similarly named bridge/deep-link values are not merely telemetry strings.  The exact Sober code that translates the WebKit request into this model is still unobserved.

The Java-to-native game-launch call is now exact.  `GameManager.launchGameWithParams` schedules the launch on the UI thread; the downstream `vi.i0` routine obtains the active `Surface`, platform/device settings, account/age state, and the request fields, fills a `StartGameParams.Builder`, calls `build()`, and directly invokes `NativeGLInterface.nativeAppBridgeV2StartGameWithParam(StartGameParams)`.  That JNI method is the Android client's immediate handoff into `libroblox.so` for a game launch.  The normal startup logs' `JNIAppBridge` / `SingleSurfaceApp::launchUGCGame` sequence is therefore the native consequence of this call path.  This proves the typed Android → `libroblox` boundary, while leaving the preceding detached WebKit socket → Android-model mapping unknown.

The payload's native code makes the surface portion of that Android boundary concrete.  Its `NativeGLInterface` JNI path calls `ANativeWindow_fromSurface` for the Java `Surface`; separate update handlers release the previous window, cache the returned `ANativeWindow*`, acquire/release it across updates, and query `ANativeWindow_getWidth`/`ANativeWindow_getHeight` to retain the current dimensions.  The direct `ANativeWindow_fromSurface` import resolves to Sober's in-memory Android namespace in the live client.  Thus the Java `Surface` becomes the Android-native window object consumed by `libroblox`; how Sober represents that object over its Linux Wayland/X11 window remains a separate, unobserved translation step.

The same manifest requests Android permissions including Internet, microphone, camera, contacts, external storage, notifications, Bluetooth, Wi-Fi/network state, biometric/fingerprint, vibration, wake lock, and audio settings.  These are capabilities expected by the Android client, not proof of matching host access: the observed Flatpak grants remain the controlling Linux policy (network, audio, graphics, selected desktop IPC, and per-app storage).  Across the sampled normal starts, the logs contain no Android permission request, grant, or denial event, so per-permission emulation/denial behavior inside Sober's Android shim remains unobserved.  They do consistently show Roblox's `LocalStorageHandler` as unavailable on this platform while its own `RbxStorage` opens SQLite/files below Sober's Flatpak data directory; that is an application-storage observation, not evidence about Android runtime permissions.

The protected `Main` child mapped:

```text
/home/pepe/.var/app/org.vinegarhq.Sober/data/sober/packages/x86_64/com.roblox.client/base.apk
/app/bin/libbadcpu.so
/app/bin/libloader.so
/app/bin/libmimalloc.so
/usr/lib/.../libEGL.so.1
/usr/lib/.../libGLESv2.so.2
/usr/lib/.../libvulkan.so.1
/usr/lib/.../libvulkan_intel.so
```

It also used anonymous/memfd-backed mappings, including `memfd:sober`, rather than a visible on-disk `libroblox.so` mapping.  `libroblox.so` is stored compressed inside `split_config.x86_64.apk`, so the live mapping is consistent with a custom loader decompressing or copying it into anonymous memory.  This is an inference from the APK layout and maps, not proof of the precise loading routine.

The runtime log establishes this startup order after the Android payload starts:

```text
AndroidGLView nativeInitClientSettings
 -> JNIAppBridge nativeAppBridgeAppStart / V2Init
 -> SingleSurfaceApp initializeWithAppStarter
 -> setAssetFolder and setExtraAssetFolder
 -> initializeLuaAppWithLoggedInUser
 -> Vulkan framebuffer/swapchain creation
 -> app_interface$json will_handle_app_startup / did_handle_app_startup
```

A fresh normal-startup log resolves this into a timed, ordered sequence.  Sober performs `global_window_init` and loads the host Vulkan libraries first.  `AndroidGLView::nativeInitClientSettings` and `nativePostClientSettingsLoadedInitialization3` follow, then `JNIAppBridge::nativeAppBridgeAppStart` and `nativeAppBridgeV2Init`.  `SingleSurfaceApp::initializeWithAppStarter`/`initializeSingleton` install the content and extra-asset folders, enter `stage:Native`, create controllers and the experience coordinator, and `nativeAppBridgeStartLuaAppDM` calls `initializeLuaAppWithLoggedInUser`.  After startup controller completion the app reaches `stage:InitializedLuaApp`; the bridge updates both Lua-app and UGC-game surfaces.  `nativeAppBridgeV2StartApp` then calls `startLuaApp`/`returnToLuaApp`, replaces the surface data model, creates the first host Vulkan swapchain, starts the `SurfaceController` render job, and transitions to `stage:LuaApp`.  Only after that transition does it emit `{"type":"did_handle_app_startup"}`.  This is a normal home-app startup, not a game-place launch; it establishes the post-`libroblox` bootstrap order without claiming that `launchUGCGame` ran.

The logs also expose a small, concrete part of the Android payload's runtime interface as compact JSON records.  The observed records are exactly `{"type":"will_handle_app_startup"}`, `{"type":"did_handle_app_startup"}`, and, after the initial Lua-app data model is available, `{"place_id":"0","type":"game_loaded"}`.  A privacy-preserving inventory across thirteen captured starts found no other `app_interface$json` type; those three records each occurred once per start.  The adjacent outer `app$json` channel emitted only `device_product_info` with the keys `product_name`, `product_version`, and `type`.  In the sampled initialization, the order is: Sober loads Vulkan and creates the native window; `AndroidGLView` initializes settings; `JNIAppBridge` and `SingleSurfaceApp` construct the app; the payload updates app/game surface parameters; it emits `will_handle_app_startup`; `nativeAppBridgeV2StartApp` returns to the Lua app; Vulkan enables `VK_KHR_surface`, `VK_KHR_get_physical_device_properties2`, and `VK_KHR_android_surface`, creates the framebuffer/swapchain and loads the mobile shader pack; `SingleSurfaceApp` enters `LuaApp`; then it emits `did_handle_app_startup`.  These records establish runtime state-notification contents, but their FD/consumer and relation to the WebKit service socket remain unobserved.

A fresh normal-run log supplies direct ordering around that summary: `global_window_init` logs “Will load Vulkan libs”, then “Loaded Vulkan libs successfully”, then creates a Vulkan (not OpenGL) native window **before** `nativeAppBridgeAppStart`/`nativeAppBridgeV2Init`.  `SingleSurfaceApp` then sets the content and extra-asset folders, starts the Lua application, publishes the Lua/game surface parameters, and starts the Lua app.  Only then does Roblox enumerate `VK_KHR_android_surface`, choose the host Intel UHD 620 Vulkan device in this environment, and create a 1920×1006 swapchain.  The same process maps host `libvulkan.so.1`, Mesa LLVM/virtio/Radeon ICD modules, Wayland client/egl/cursor libraries, and X11/XCB libraries.  This verifies a Linux native-window → Android-facing Vulkan sequence, but the protected process does not permit a read-only FD listing here, so it still does not prove whether this swapchain used Sober's Wayland- or Xlib-surface branch.

When a game is selected, logs show:

```text
app_interface$json will_handle_start_game
 -> SingleSurfaceApp::launchUGCGame
 -> launchUGCGameInternal
 -> app_interface$json did_handle_start_game
 -> app_interface$json game_loaded
```

This confirms that the Android Roblox payload, rather than `sober_services`, owns the actual game launch sequence.  In two normal Android-client startups, no live `sober_services` process was observed after startup while the full Roblox home UI initialized successfully.  In a later normal launch, however, the helper was live as `/app/bin/sober_services --server 22 detached`.  Its server FD was a Unix stream socket and the outer `sober` process was its ptrace tracer; protected FD-table access prevented direct confirmation of the socket peer.  The reconciled conclusion is that the helper is an optional/on-demand WebKit bridge, not the process which starts the Android game runtime.

The installed desktop action supplies a second supported invocation: `sober config` starts `/app/bin/sober_services --server 21 attached` (the decimal descriptor varies per launch).  The descriptor is again a Unix stream socket.  This establishes the final mode argument precisely: `attached` is used for the settings action and `detached` for the earlier ordinary bridge instance.  Neither invocation starts Roblox as a separate helper executable; the game-capable `Main` process is already a Sober process.  A helper-only preload probe with a constructor marker did not load in this child even though it loaded in the outer launch, proving Sober sanitizes `LD_PRELOAD` before spawning `sober_services`; this is why a bridge-write event has not yet been captured by that method.

### Loaded-object order and Rust attribution

At native startup, `sober` has bundled `libloader.so`, `libmimalloc.so`, and `libbadcpu.so` among its base ELF dependencies, and resolves EGL/GLES from the Flatpak runtime.  Its constructor order is given precisely below; dependency-list order must not be confused with constructor order.  The Android child subsequently loads the Vulkan loader and the available Mesa ICDs, including Intel, lavapipe, nouveau, Radeon, virtio, and hasvk candidates.  It selected/used the host Intel Vulkan path on this machine.

glibc loader diagnostics refine the startup order: dependency search finds bundled `libloader.so` and `libmimalloc.so` first, resolves host EGL/GLES after the `$ORIGIN` lookup fails, initializes `libbadcpu.so`, then initializes `libmimalloc.so`, then `libloader.so`.  `libloader.so` subsequently loads `libseccomp.so.2` on demand.  This is the authoritative dynamic-linker order for the native outer process; it does not expose custom Android payload objects loaded by Sober after its own runtime begins.

The audit interface provides the corresponding **complete object-open order for the observed outer process image** (each entry is emitted by `la_objopen`, not inferred from `DT_NEEDED`):

```text
<main>, ld-linux-x86-64.so.2, linux-vdso.so.1,
/app/bin/libloader.so, /app/bin/libmimalloc.so,
libcrypto.so.3, libcurl.so.4, libfreetype.so.6, libfontconfig.so.1,
libz.so.1, libsecret-1.so.0, libgobject-2.0.so.0, libglib-2.0.so.0,
libxml2.so.16, libGLESv2.so.2, libEGL.so.1, libgstreamer-1.0.so.0,
libgstapp-1.0.so.0, libgstvideo-1.0.so.0, libdbus-1.so.3, libm.so.6,
libgcc_s.so.1, libc.so.6, /app/bin/libbadcpu.so, libidn2.so.0,
libssl.so.3, libzstd.so.1, libnghttp2.so.14, libpsl.so.5, libbz2.so.1,
libpng16.so.16, libharfbuzz.so.0, libbrotlidec.so.1, libexpat.so.1,
libgio-2.0.so.0, libgcrypt.so.20, libffi.so.8, libpcre2-8.so.0,
libicuuc.so.77, libGLdispatch.so.0, libgmodule-2.0.so.0, libunwind.so.8,
libdw.so.1, libgstbase-1.0.so.0, liborc-0.4.so.0, libsystemd.so.0,
libunistring.so.5, libgraphite2.so.3, libbrotlicommon.so.1, libmount.so.1,
libgpg-error.so.0, libicudata.so.77, libstdc++.so.6, liblzma.so.5,
libelf.so.1, libblkid.so.1, libeconf.so.0; then libseccomp.so.2.
```

This is exact for the audited outer native process for the sampled startup.  It deliberately does not claim an order for `libroblox.so`: that Android payload is decrypted into a deleted memfd and is not exposed as a conventional loader object to this audit path.

`Investigation/audit_probe.c` adds an `LD_AUDIT` observation path that runs before ordinary constructors and does not use ptrace.  In a successful normal startup it observed loading of the bundled loader/mimalloc/badcpu objects and the host dependencies, then bindings for `memfd_create`, `fork`, `execve`, `execvp`, `posix_spawn`, `posix_spawnp`, `pipe2`, `socketpair`, `recvmsg`, `read`, `poll`, `epoll_wait`, `dlopen`, `dlsym`, and `syscall`; it also observed the later load of `libseccomp.so.2`.  This proves that `memfd_create` is used live even though it is not a direct outer-ELF import.  Its redacted wrappers for `execvp`, `execve`, and `posix_spawn*` did not see a launcher call in the sampled startup, so they have not yet revealed a new command line or environment.

The same module was extended to record successful `socketpair`/`pipe2` calls and the return values of selected raw process/IPC syscalls, without recording arguments, messages, or environment values.  In a second normal startup neither hook fired while `Main` and its children were created.  This negative result is bounded: it rules out only those interposable routes for this startup; it does not rule out direct assembly, a protected payload, or an earlier bootstrap path.

It then redirected the `ptrace` PLT entry and dynamic `dlsym` results for `ptrace`, `fork`, `exec*`, `socketpair`, and `pipe2`.  A third normal startup bound `ptrace` but did not call either interception path.  The outer on-disk `sober` image is packed after its early executable segment—most raw `0f 05` hits decode as data—so it cannot currently supply a trustworthy static call site either.

Rust is now directly evidenced, not merely suspected:

- `libbadcpu.so` embeds standard Rust panic, backtrace, TLS, and `__rust_*` runtime strings;
- `libloader.so` embeds `RUST_LIB_BACKTRACE` and Rust debug/backtrace metadata;
- Rizin assigns `libbadcpu.so` the concrete responsibility of x86-64-v2/SSE feature checking and a SIGILL-based SSE 4.2 fallback; and
- `libloader.so` contains the separately recovered generic child-process/identity/`execvp` primitive.

Both are stripped, so their remaining public responsibility boundaries cannot yet be assigned at function granularity.

`sober_services` remains best characterized as a native GTK/WebKit helper (C++/GLib ecosystem); no comparable Rust runtime evidence was recovered from it.

### Android API boundary recovered from `libroblox.so`

The x86-64 Android `libroblox.so` has 73 Android-specific unresolved imports.  This is a linker-level inventory of what Sober must provide, redirect, or otherwise make available to the Android payload:

| Area | Imported Android API families |
| --- | --- |
| Asset/package access | `AAssetManager_*`, `AAsset_*`, `AConfiguration_*` |
| Android event loop | `ALooper_*` |
| Window/surface | `ANativeWindow_*` |
| Device properties and logging | `__system_property_get`, `__android_log_*`, `android_set_abort_message` |
| Media | `AMediaCodec_*`, `AMediaFormat_*` |
| Audio | OpenSL ES interface IDs including `SL_IID_ANDROIDCONFIGURATION` and `SL_IID_ANDROIDSIMPLEBUFFERQUEUE` |
| Graphics | Android-facing EGL/GLES imports plus Vulkan usage at runtime |

The same ELF declares Android NEEDED libraries `libandroid.so`, `liblog.so`, `libmediandk.so`, `libOpenSLES.so`, and `libOpenMAXAL.so`, in addition to Android `libc`, `libdl`, EGL, and GLES.  In the live protected Linux process, the mapped files included the host EGL/GLES/Vulkan libraries and the bundled Sober objects, but no pathname-backed copy of those Android system libraries.  Together with the Android `/system/bin/linker64` expectation in `libtrampoline.so`, this is direct evidence for a custom Android dynamic-linking/compatibility layer rather than a conventional Linux `dlopen` of `libroblox.so`.

A later read-only live mapping snapshot sharpened that result.  The Android `Main` child had pathname-backed mappings for `/app/bin/libbadcpu.so`, `/app/bin/libloader.so`, `/app/bin/libmimalloc.so`, host `libEGL.so.1`, host `libGLESv2.so.2`, `libvulkan.so.1`, and Mesa Vulkan ICDs.  It had no pathname-backed `libandroid.so`, `liblog.so`, `libdl.so`, `libmediandk.so`, or OpenSL/OpenMAX library.  It also had deleted `/memfd:sober` mappings and many anonymous executable `memfd` mappings.  Comparing the `memfd:sober` offsets to the on-disk outer ELF shows that its executable mapping begins at the same page-aligned `0x1e5000` offset as `sober`'s main `PT_LOAD` code segment; this object is therefore Sober's self-remapped runtime image, not proof that it is `libroblox.so`.  The remaining anonymous objects may include the Android payload or substitute library namespace, but their ownership is still unproven.  The absence of pathname-backed Android libraries remains direct evidence that ordinary host-library resolution is not the compatibility mechanism.

A later process-name-aware map snapshot assigns the anonymous client image: the traced process with `Name: Main` (whose parent is the untraced outer `sober`) alone maps repeated RX/RW views of one unnamed deleted memfd, with a zero-offset RX segment followed by a `0x689e000` data segment and a total layout matching the extracted x86-64 `libroblox.so`; its two `sober` worker children map only `memfd:sober`.  The live anonymous client image retains a valid ELF header and `PT_DYNAMIC`, but its `DT_STRTAB`/`DT_STRSZ` values have been cleared by the sampling point.  Consequently ordinary post-load ELF relocation enumeration cannot name its live import providers, even though the original APK ELF and earlier pre-scrub relocation captures establish its Android import surface.  This is direct evidence of post-relocation metadata scrubbing, not evidence that the payload is absent.

Read-only `process_vm_readv` metadata inspection now identifies the large anonymous Android payload precisely.  A valid ELF64 header in a `/memfd:` executable mapping has SONAME `libroblox.so`, and its live `DT_NEEDED` list is `libOpenMAXAL.so`, `libmediandk.so`, `libOpenSLES.so`, `libGLESv2.so`, `libEGL.so`, `libandroid.so`, `liblog.so`, `libm.so`, `libdl.so`, and `libc.so`.  The image is about `0x689e3f0` bytes in its first load segment and is backed by a deleted memfd.  The process contains twelve large RX/RW mapping pairs with the **same memfd inode** and offset zero; these are repeated mappings of this one `libroblox.so` image, not a collection of named Android shim libraries.  One mapping instance includes the separate read-only/dynamic segments at the expected offsets, allowing the SONAME recovery.  This directly locates the Android client in memory and further narrows the compatibility implementation to Sober's self-remapped/unpacked code or anonymous native regions rather than another discoverable memfd ELF.

Two bounded anonymous maps now expose Sober's synthetic Android library namespace.  A read-only `0xc0000` map contains the sonames `libandroid.so`, `liblog.so`, `libmediandk.so`, `libOpenSLES.so`, `libOpenMAXAL.so`, together with the Android-facing `libEGL.so`, `libGLESv2.so`, and the expected `libc.so`/`libdl.so`.  A separate writable `0x30000` map contains the corresponding Android API-name table: asset APIs (`AAssetManager_*`, `AAsset_*`, `AAssetDir_*`), configuration APIs (`AConfiguration_*`), looper/native-window APIs (`ALooper_*`, `ANativeWindow_*`), Media NDK APIs (`AMediaCodec_*`, `AMediaFormat_*`), Android log APIs (`__android_log_*`), and both `vkCreateAndroidSurfaceKHR` and `vkGetInstanceProcAddr`.

The writable registry contains two distinct `vkCreateAndroidSurfaceKHR` name records.  A bounded `process_vm_readv` search found neither same-map pointers nor direct pointers to either record in the other readable mappings of the protected process.  It is therefore not a retained conventional `{name, function-address}` table; Sober likely hashes, copies, or otherwise consumes the names during construction.  This rules out deriving the Android-surface function address merely by following an adjacent pointer, while leaving its implementation owner unproved.

This is direct runtime evidence that Sober builds an in-memory Android soname/symbol namespace for the imports in `libroblox.so`; it is no longer only an inference from absent pathname-backed libraries.  The table identifies the **replaced/intercepted API surface**, including the Android Vulkan entry point missing from the host loader.  It does not, by itself, identify the code address implementing each entry or prove whether a given entry is a full emulation, thin Linux translation, or a stub; those implementation assignments remain open.

Live relocation resolution assigns the implementation owner for the directly imported Android surface.  Reading the resolved GOT entries of the in-memory `libroblox.so` shows that all of these point into Sober's self-remapped executable mapping, `/memfd:sober`, rather than a host library or a separate fake ELF:

| API family | Resolved examples | Provider |
| --- | --- | --- |
| Assets | `AAssetManager_fromJava`, `AAssetManager_open` | `/memfd:sober` |
| Configuration | `AConfiguration_new`, `AConfiguration_getLanguage` | `/memfd:sober` |
| Event/window | `ALooper_pollOnce`, `ANativeWindow_acquire`, `ANativeWindow_fromSurface` | `/memfd:sober` |
| Properties/logging | `__system_property_get`, `__android_log_print`, `__android_log_write`, `android_set_abort_message` | `/memfd:sober` |
| Media NDK | `AMediaCodec_createDecoderByType`, `AMediaFormat_new` | `/memfd:sober` |
| Audio | `slCreateEngine` | `/memfd:sober` |
| Signal/TID boundary | `sigaction`, `pthread_sigmask`, `gettid`, `syscall` | `/memfd:sober` |

This establishes the principal Android compatibility implementation location: the self-remapped/unpacked main Sober image supplies the direct imported APIs.  `vkCreateAndroidSurfaceKHR` and `vkGetInstanceProcAddr` are present in the synthetic symbol-name table, but a fresh live relocation walk found no `libroblox.so` GOT relocation for either symbol (nor for `vkCreateInstance`/`vkGetDeviceProcAddr`).  The original x86-64 APK `libroblox.so` now resolves the ambiguity on the client side: its Vulkan dispatch initializer calls its `vkGetInstanceProcAddr` pointer with the literal name `vkCreateAndroidSurfaceKHR`, stores the returned address in its private dispatch table (`+0x710c2f0` in this build), and later calls that cached pointer.  The call site constructs a standard `VkAndroidSurfaceCreateInfoKHR`: `sType = 1000008000` (`VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR`), null `pNext`, zero flags, and its incoming Android-window pointer copied unchanged to `window`; it then invokes `vkCreateAndroidSurfaceKHR(instance, &surfaceInfo, nullptr, &wd.presentationSurface)`.

A bounded post-initialization read confirms that this slot is populated only after Vulkan startup.  Applying the actual split-`PT_LOAD` mapping bias (the writable/dynamic view starts at virtual page `0x68a2000`, not file offset `0x689e000`) locates the live slot correctly.  Four seconds after `Main` appeared—after the log had enabled `VK_KHR_android_surface` and created the swapchain—the first word at the slot was `0x0000555d64d882a0`; it was zero immediately at `Main` creation.  This is the first concrete runtime target returned by the Android-surface resolver.  A same-run mapping snapshot then places it in the executable `/memfd:sober` range, at image-relative address `0x4392a0`; a bounded read begins `f3 0f 1e fa 41 57 41 56 53 48 83 ec 70 48 89 fb`, a normal CET-enabled function prologue.  This conclusively assigns the Android-surface entry point to Sober's unpacked runtime, rather than the host Vulkan loader or `libroblox.so`.

The live code also resolves the backend exactly.  The Android entry first validates `VkAndroidSurfaceCreateInfoKHR.window`, unwraps Sober's native-window object, and forwards the original Vulkan instance, allocator, and output-surface arguments to an internal dispatcher at `entry + 0x214af0`.  That dispatcher tail-calls a runtime-selected backend through a context function pointer at `+0x210`; the selected backend is again in `/memfd:sober`.  Its first action obtains a host Vulkan entry through the context's `vkGetInstanceProcAddr`-style callback at `+0x560`.  It then constructs this standard `VkWaylandSurfaceCreateInfoKHR` on its stack and calls that host function:

```text
sType   = 1000006000  (VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR)
pNext   = NULL
flags   = 0
display = *([native_window + 0x188] + 0x08)
surface =  [native_window + 0x188] + 0x10
```

The forwarded call preserves the Android call's `VkInstance`, allocator, and output pointer, replacing only the Android create-info with the Wayland create-info.  Thus Sober implements `VK_KHR_android_surface` by translating it to **host `vkCreateWaylandSurfaceKHR`**, backed by its own native-window object's Wayland display/surface.  This is direct live instruction evidence, not an inference from loaded libraries.  The exact construction/lifetime of that native-window object and the fallback behavior when Wayland is unavailable remain unobserved.

Signal handling is therefore selective rather than a wholesale signal emulation layer.  In the same live `libroblox.so` relocation map, `getpid`, `signal`, `raise`, and `pthread_kill` resolve to host libc, whereas `gettid`, `sigaction`, `pthread_sigmask`, and the variadic `syscall` gateway resolve to Sober's decrypted runtime.  The first instructions at each latter target are real runtime code (not a PLT-style jump stub), establishing executable interception at this boundary; `sigaction` begins with a tiny immediate return path while the other three have nontrivial wrapper prologues.  What those wrappers do for each signal number or syscall—and whether they translate, filter, or merely adapt the ABI—remains unobserved.

The same live relocation walk distinguishes the graphics boundary rather than treating all graphics calls as one layer.  The direct EGL imports inspected (`eglGetDisplay`, `eglInitialize`, `eglChooseConfig`, `eglCreateContext`, `eglCreateWindowSurface`, `eglMakeCurrent`, `eglSwapBuffers`, `eglGetProcAddress`, and related lifecycle/query calls) resolve to `/memfd:sober`: Sober wraps the Android-facing EGL ABI.  The ordinary GLES entry points inspected (`glGetError`, `glUseProgram`, shader, buffer, texture, and draw APIs) resolve directly to the Flatpak host `libGLESv2.so.2.1.0`.  `glGetString` initially resolves to a Sober CET trampoline, but its live indirect slot points straight to host `libGLESv2.so.2.1.0`; it is ABI dispatch plumbing, not evidence of string rewriting.  This is direct evidence for an EGL/window boundary plus pass-through GLES dispatch, not an all-emulated GL stack.

A bounded live disassembly establishes the mechanism behind the EGL function lookup.  The resolved `eglGetProcAddress` target lies in `/memfd:sober`, copies the requested symbol name into an owned buffer, looks it up through a Sober-internal hash-table registry rooted at image-relative `0x745050`, and returns the stored function pointer at entry offset `+0x28` when present.  It does not call the host `eglGetProcAddress` PLT entry on this path.  In a normal-startup read-only snapshot, the active lookup container fields at registry offsets `+0x10` and `+0x18` were respectively null and zero, so no dynamic entry was present at that instant.  The lookup table also has no direct pointers to the two synthetic `vkCreateAndroidSurfaceKHR` name records.  Therefore Sober has a dynamic in-memory graphics dispatch mechanism, rather than relying solely on ELF symbol resolution, and it is a plausible implementation site for Android-only entry points.  This proves the lookup mechanism, not yet the registry entry or translation body for `vkCreateAndroidSurfaceKHR`.

The self-remapped image is demonstrably **decrypted at runtime**.  For example, the live resolved `__system_property_get` entry begins with a normal CET/x86-64 function prologue (`endbr64`, register saves, and stack allocation); the on-disk `sober` bytes at the corresponding `PT_LOAD` offset are unrelated ciphertext.  The normal path validates both pointers, copies the requested key, and calls an internal lookup routine.  Its null-argument error path contains further runtime-literal decryption; that is evidence of protected diagnostics, not proof that the property table itself is encrypted.  This explains why Rizin/Ghidra on the installed outer ELF could not recover the Android shim statically: the definitive implementation is available only in the live `/memfd:sober` code mapping.  It also proves properties are handled through Sober's in-process resolver rather than by opening a visible Android property file; the exact key/value lookup rules remain to be recovered from that live code.

The already-decrypted runtime image supplies a concrete, bounded **candidate** Android-property vocabulary.  It contains `ro.hardware`, `ro.crypto.state`, `ro.soc.manufacturer`, `ro.product.{brand,device,vendor.manufacturer,model,name,board,manufacturer}`, `ro.product.cpu.abilist`, `ro.build.{type,version.codename,user,display.id,version.release,host,flavor,version.sdk,characteristics}`, `ro.arch`, `ro.kernel.{qemu,android.qemud}`, `ro.bootloader`, `persist.sys.timezone`, and `ro.vendor.build.version.incremental`.  Together with the live Sober-owned resolver, this is strong evidence for Android-style hardware/product/build property support, but string presence alone does not prove that every key reaches the lookup routine or what it returns.  Some nearby readable strings look like plausible defaults, but their individual key/value pairing is not statically established; the report deliberately does not claim returned values until the normal lookup table is recovered.

Rizin identifies the normal lookup as a string-keyed in-memory hash map in the **captured decrypted runtime image**: in that image it is rooted at image-relative `0x744f10`, its initializer at `0x2ed070` creates the map header and calls the insertion routine at `0x41d280` exactly **31 times**.  Each input record contains three owned strings and a 32-bit field.  The inserter allocates a `0x78`-byte node and copies those strings into node slots rooted at `+0x08`, `+0x28`, and `+0x48`, with the allocation rounded to a visible `0x80`-byte stride.  A fresh post-startup resident-page scan now corrects the earlier static field interpretation: the live nodes contain the property key in both the `+0x08` and `+0x28` string slots and the returned synthetic value in the `+0x48` string slot.  The field at `+0x68` remains auxiliary metadata/flags.  A read-only normal-startup snapshot of the nominal image-relative header (`0x744f00`–`0x744f3f`) was all zero, as was a later sample after swapchain creation; the live heap nodes prove that this per-launch runtime does not retain the active map at that nominal cross-launch address.

Several key/value pairs are nevertheless directly visible in that initializer's adjacent stack construction.  These strings are quoted exactly as constructed, including bracket characters:

| Android property | Constructed value |
| --- | --- |
| `ro.kernel.qemu` | empty string |
| `ro.crypto.state` | `[unsupported]` |
| `ro.bootloader` | `[unknown]` |
| `ro.build.characteristics` | `[default]` |
| `ro.build.type` | `[userdebug]` |
| `ro.product.cpu.abilist` | `[x86_64,x86]` |
| `ro.build.user` | `[jenkins]` |
| `ro.build.version.release` | `10` |
| `ro.soc.manufacturer` | `Samsung` |
| `ro.product.manufacturer` | `samsung` |
| `ro.product.brand` | `waydroid` |
| `ro.product.board` | empty string |

The final four rows are direct live-heap records from the same initialized `0x78`-byte node layout rather than associations inferred from adjacent literals.  `Investigation/live_present_string_scan.c` finds exact strings only in resident writable pages by consulting `/proc/<pid>/pagemap`; a bounded read around the matching node then exposes both duplicate key slots and the adjacent value slot without debugger attachment or process modification.

This identifies the required API surface, but not yet the implementation owner for every symbol.  The likely owners are the custom payload loader and runtime code in `sober`/`libloader.so`; that assignment remains an inference until symbol-resolution or loader tracing can be captured.

### Remaining hard limits

The `executeRoblox` envelope is now known from the cached, actually-loaded Hybrid JavaScript, its helper-side framing is a fixed opcode-10 packet, and one central opcode-10 JSON parser is identified.  Still unverified are the particular live `params.request` fields sent by a launch click and the detached-game command-specific validation/dispatch after parsing.  The complete recovered key set of the identified reader is configuration/UI-only, so it is not the detached game dispatcher.  A normal protected child cannot be safely attached or have its FD table/environ read from outside: doing so changes the tracer relationship that Sober requires.  Capturing a concrete record safely will require a bridge event with a scoped helper-side probe or a trace mechanism compatible with Sober's parent-controlled tracing model.

Consequently, these specific implementation facts remain unknown rather than merely inferred:

- the accepted command-specific `params` schemas and the central validation/dispatch after the still-unlocated detached-game JSON parser;
- the actual `argv`, environment, and library-record set used for any `libloader.so` child launch in the normal Roblox path;
- the exact custom linker/namespace rules and the individual implementation behavior behind the Android APIs; their direct `libroblox.so` imports are now known to resolve into `/memfd:sober`;
- whether the normal Android runtime process is made by the outer `sober` executable directly, through `libloader.so`, or through a payload loader reached later;
- the non-interposable process-creation mechanism that forms `sober → Main → sober` children, and whether it also establishes the parent-controlled ptrace relationship;
- the full ptrace protocol between `sober` and its `Main` child, including which requests, events, and policy checks it uses;
- the semantic responsibilities of the Rust code in `libloader.so` and `libbadcpu.so` beyond their directly observed generic launching and host/CPU inspection behavior.

An additional static pass found that the helper registers both message channels by name with the WebKit content manager, then registers the two corresponding script-message handler names.  There is no embedded WebKit JavaScript resource containing the command schema: the standard GLib resource section is absent from the stripped executable, and the only identifiable resource is the GTK onboarding UI.  The command is therefore likely supplied by the loaded web content rather than a recoverable local script.

The installed Flatpak manifest grants only `dri`, shared IPC/network, Wayland/X11, PulseAudio, and the Flatpak `devel` feature.  It has no extra host-filesystem permission.  Thus Sober's persistent Android-style storage is supplied through Flatpak's per-application data/cache/config binds, not a broad host directory mapping.

### Filesystem and proc-path boundary observed live

`libroblox.so` contains literal references to Linux pseudo-files such as `/proc/self/status`, `/proc/%d/maps`, `/proc/cpuinfo`, `/proc/meminfo`, and `/sys/devices/system/cpu/...`; it also contains Android-style references including `/sdcard/Android/data`, `/system/fonts/NotoSansCJK-Regular.ttc`, and `/system/bin/app_process64`.  A read-only snapshot of the actual traced `Main` process settles the normal-path filesystem layout: its root is the Flatpak/bubblewrap tmpfs root, with `/usr` and `/app` read-only runtime/app mounts, writable per-app mounts at `/var/data`, `/var/cache`, `/var/config`, and `/var/tmp`, Linux `/proc`, and selected read-only `/sys` subtrees.  There are **no** `/sdcard`, `/system`, `/vendor`, `/apex`, `/data`, `/storage`, or `/mnt` entries and no mounts at those paths.

Accordingly, Sober does not establish a conventional Android root filesystem or path-level bind mapping in this normal client process.  Roblox's `/proc` and `/sys` reads are handled directly by the Linux pseudo-filesystems (subject to Flatpak's filtered mounts); persistent app data uses Sober/Roblox's configured Linux storage under Flatpak rather than a visible `/data/data` or `/sdcard` tree.  The Android-looking literal paths may be fallback, development, or error paths, or may be translated in a still-unobserved API shim; their presence alone is not proof that they resolve successfully.

`Investigation/path_probe.c` was built as a constructor-marked, read-only `open`/`openat` interposer that would record only platform paths and return codes.  A normal launch with `LD_PRELOAD` set to the probe produced no constructor marker, so this is an unambiguous environment-sanitization result—not evidence that Roblox made no path calls.  It corroborates the helper result: the normal Sober bootstrap removes `LD_PRELOAD` before entering its self-remapped runtime and before spawning `sober_services`.  Further path/property tracing therefore requires an audit-compatible internal hook or a capture within the packed runtime, rather than conventional preload interposition.

The controlled `Main` child additionally denies direct `/proc/<pid>/environ` access even to the launcher shell.  `Investigation/env_names_probe.c` reads only the initial stack through permitted `process_vm_readv` and emits **variable names only**, never values.  It confirms that the child retains ordinary Flatpak/session variables but has no `LD_PRELOAD`, `LD_AUDIT`, `LD_DEBUG`, or `LD_LIBRARY_PATH`; the sole observed `LD_*` name is `LD_BIND_NOW`.  This is direct confirmation of loader-instrumentation sanitization while avoiding collection of credentials or other environment values.  It still cannot establish the full value-level environment construction.

### Non-perturbing IPC-probe validation

`Investigation/ipc_probe.c` is a scoped `LD_PRELOAD` probe that intercepts only `write()` and `writev()` from `/app/bin/sober_services` to non-stdio FIFO or Unix-socket descriptors.  The latter is relevant because `writev` is the helper's only recoverable low-level output import.  It recognizes only an exact full `0x2830`-byte opcode-10 record (reassembling at most 32 iovecs locally) and logs its NUL-bounded JSON payload beginning at offset 8; all application writes and vectors are otherwise forwarded unchanged.  The probe compiles cleanly to `libsober_ipc_probe.so` and a 12-second normal Sober startup under the preload reached `did_handle_app_startup`, demonstrating that preload instrumentation does not itself trigger the ptrace security check.  The socket acceptance was added after a live helper instance proved that its `--server` FD is a Unix stream socket.

`Investigation/bridge_roundtrip.c` is a separate controlled socket-pair validation harness.  Given a newly started central Sober PID and its peer descriptor, it pauses only that central process, asks the helper to evaluate a local JavaScript sentinel, and uses `MSG_PEEK` on the duplicated central endpoint.  It never consumes queued traffic.  Its acceptance condition checks the actual frame: `0x2830` bytes, opcode `0x0a`, and sentinel JSON at offset 8; it deliberately ignores bytes 1--7 and the tail.  An isolated `dbus-run-session` helper/socket-pair test independently produced a full `10288`-byte reply with opcode `10` and the sentinel at offset 8, while proving that bytes 1--7 are not reliably zero.

No `sober_services` process and no candidate IPC record appeared during that startup, so there was no `executeRoblox` event to capture.  The grammar is now recovered statically; a message-bearing detached-mode capture still requires a user action that opens the WebKit bridge or another reproducible path, and does not require external ptrace.
