'use strict';

/* Read-only timing probe for the two proven libroblox render-path offsets.
 * Offsets are image-relative for the currently launched APK and must be
 * revalidated after a Roblox APK update.  It reports aggregate call count,
 * total time, maximum time, and long calls; it never reads game data. */
const targets = [
  { name: 'render_frame_region', offset: 0x247a4d6 },
  { name: 'endRender_region', offset: 0x24eb184 },
];
const counters = {};
/* Nuah's Android loader maps libroblox as rwx, so Frida does not expose it as
 * a regular Module.  Locate its file-backed executable range instead. */
const robloxRange = Process.enumerateRanges('rwx').find(range =>
  range.file !== undefined && range.file.path.includes('libroblox.so'));
if (robloxRange === undefined) {
  send({ error: 'libroblox.so is not loaded' });
} else {
  const roblox = { base: robloxRange.base };
  for (const target of targets) {
    const address = roblox.base.add(target.offset);
    counters[target.name] = { calls: 0, totalUs: 0, maxUs: 0, longCalls: 0 };
    Interceptor.attach(address, {
      onEnter() { this.startUs = Date.now() * 1000; },
      onLeave() {
        const elapsedUs = Date.now() * 1000 - this.startUs;
        const counter = counters[target.name];
        counter.calls++;
        counter.totalUs += elapsedUs;
        counter.maxUs = Math.max(counter.maxUs, elapsedUs);
        if (elapsedUs >= 20000) counter.longCalls++;
      }
    });
  }
  send({ targets: targets.map(t => ({ name: t.name, address: roblox.base.add(t.offset).toString() })) });
  const report = () => {
    send({ counters });
    for (const counter of Object.values(counters)) {
      counter.calls = 0; counter.totalUs = 0; counter.maxUs = 0; counter.longCalls = 0;
    }
    setTimeout(report, 1000);
  };
  setTimeout(report, 1000);
}
