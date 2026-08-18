'use strict';

/*
 * Read-only late-attach probe for the current libroblox.so build.
 *
 * TextureManager's stripped processPendingRequests body starts at the first
 * address below.  The nearby marker is its scoped Render profiler label.
 * This probes execution only; it changes neither engine state nor arguments.
 * Update both offsets only after validating the ELF Build ID.
 */
const queueEntryOffset = 0x2492fec;
const pendingMarkerOffset = 0x24962be;

function findRobloxBase() {
  for (const range of Process.enumerateRanges('---')) {
    if (range.file !== undefined &&
        range.file.path.endsWith('/libroblox.so') &&
        range.file.offset === 0) {
      return range.base;
    }
  }
  return null;
}

function startsWith(address, expected) {
  const actual = new Uint8Array(address.readByteArray(expected.length));
  return expected.every((value, index) => actual[index] === value);
}

function attach() {
  const base = findRobloxBase();
  if (base === null) return false;
  const entry = base.add(queueEntryOffset);
  const marker = base.add(pendingMarkerOffset);
  // push rbp; mov rbp,rsp; push r15/r14/r13/r12/rbx; sub rsp,0x5a8
  if (!startsWith(entry, [0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41,
                          0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48,
                          0x81, 0xec])) {
    send({kind: 'error', message: 'queue entry signature mismatch'});
    return true;
  }
  const counters = { entries: 0, marker: 0 };
  Interceptor.attach(entry, { onEnter() { ++counters.entries; } });
  Interceptor.attach(marker, { onEnter() { ++counters.marker; } });
  send({kind: 'ready', entry: entry.toString(), marker: marker.toString()});
  setInterval(function () {
    send({kind: 'texture_queue', entries: counters.entries,
          processPendingMarkers: counters.marker});
    counters.entries = 0;
    counters.marker = 0;
  }, 1000);
  return true;
}

const timer = setInterval(function () {
  if (attach()) clearInterval(timer);
}, 10);
