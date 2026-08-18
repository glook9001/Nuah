'use strict';

/*
 * One-shot diagnostic for the current libroblox.so image.
 *
 * It observes Roblox's JNI integer-flag registration and emits only names
 * related to texture streaming/transcoding. It never changes an argument or
 * return value. Launch Nuah with NUAH_PAUSE_BEFORE_INIT=1, attach this script
 * to the stopped process, then send SIGCONT: this sees registration before
 * the client-settings response is applied without Frida spawn injection.
 */

const interesting = /(texture|mip|transcode|stream|residen|render.*quality|quality.*render)/i;
const pointerSize = Process.pointerSize;

function jstringToUtf8(env, value) {
  if (value.isNull()) return null;
  // JNI function table entries: GetStringUTFChars=169,
  // ReleaseStringUTFChars=170. This is the stable JNI 1.6 layout on x86_64.
  const table = env.readPointer();
  const getCharsAddress = table.add(169 * pointerSize).readPointer();
  const releaseCharsAddress = table.add(170 * pointerSize).readPointer();
  const getChars = new NativeFunction(getCharsAddress, 'pointer',
                                      ['pointer', 'pointer', 'pointer']);
  const releaseChars = new NativeFunction(releaseCharsAddress, 'void',
                                          ['pointer', 'pointer', 'pointer']);
  const copied = Memory.alloc(1);
  copied.writeU8(0);
  const chars = getChars(env, value, copied);
  if (chars.isNull()) return null;
  try {
    return chars.readUtf8String();
  } finally {
    releaseChars(env, value, chars);
  }
}

function findRobloxBase() {
  const ranges = Process.enumerateRanges('---');
  for (let i = 0; i < ranges.length; ++i) {
    const range = ranges[i];
    if (range.file !== undefined &&
        range.file.path.endsWith('/libroblox.so') &&
        range.file.offset === 0) {
      return range.base;
    }
  }
  return null;
}

function attachFlagProbe() {
  const base = findRobloxBase();
  if (base === null) {
    send({kind: 'error', message: 'libroblox.so mapping not found'});
    return;
  }
  // ELF symbol value from this image's dynamic table.  The probe validates
  // the mapped file identity by path; it changes no executable bytes.
  const hooks = [
    {kind: 'fint', offset: 0x29ac0ad,
     value(args) { return args[3].toInt32(); }},
    {kind: 'fflag', offset: 0x2159802,
     value(args) { return args[3].toInt32() !== 0; }},
    {kind: 'fstring', offset: 0x29ac1ae,
     value(args) { return jstringToUtf8(args[0], args[3]); }}
  ];
  send({kind: 'ready', base: base.toString(), hooks: hooks.map(h => h.kind)});
  for (const hook of hooks) {
    const address = base.add(hook.offset);
    Interceptor.attach(address, {
      onEnter(args) {
        try {
          const name = jstringToUtf8(args[0], args[2]);
          if (name !== null && interesting.test(name)) {
            send({kind: hook.kind, name: name, defaultValue: hook.value(args)});
          }
        } catch (error) {
          send({kind: 'error', message: hook.kind + ': ' + String(error)});
        }
      }
    });
  }
}

let attached = false;
const timer = setInterval(function () {
  if (attached) return;
  if (findRobloxBase() !== null) {
    attached = true;
    attachFlagProbe();
    clearInterval(timer);
  }
}, 10);
