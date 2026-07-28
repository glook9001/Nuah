# DBus Intent API

Cross-application Android Intent delivery is implemented via the Freedesktop [D-Bus Activation](https://specifications.freedesktop.org/desktop-entry-spec/latest/dbus.html) interface. Android-style Intent objects are serialized and transmitted over DBus using the `ActivateAction` method of `org.freedesktop.Application`. The existing Freedesktop interface was reused to ensure compatibility with other Freedesktop interfaces such as `org.freedesktop.portal.Notification`, which delegates `app.`-prefixed action activations to this method. 

The sender invokes `ActivateAction` on the DBus name of the target application. This name typically mirrors the Android package name, but may differ, especially under sandboxing constraints (e.g., Flatpak). In Flatpak, exported DBus names must begin with the application ID, which must contain at least two dot-separated components (e.g., `org.example.App`).

The `action_name` argument must be one of: `startActivity`, `startService`, or `sendBroadcast`. The `parameter` argument must be a tuple of DBus type `(sssa{sv}s)` representing:

```
(action: s,
 className: s,
 data: s,
 extras: a{sv},
 sender_dbus_name: s)
```

- `action`, `className`, `data`: correspond to Android Intent fields.
- `extras`: variant dictionary containing supported primitive types: string, int32, int64, bytestring.
- If `extras` includes Parcelable values, these must be serialized as DBus tuples. The structure and interpretation of these tuples is implementation-defined and must be shared between sender and receiver out-of-band.

## Example cmdline invocation

```bash
gdbus call --session --dest org.schabi.newpipe --object-path /org/schabi/newpipe --method org.freedesktop.Application.ActivateAction "startActivity" "[<('android.intent.action.VIEW','org.schabi.newpipe.RouterActivity','https://youtube.com',{'extra_key': <'extra_value'>}, 'my.dbus.Name')>]" []
```
