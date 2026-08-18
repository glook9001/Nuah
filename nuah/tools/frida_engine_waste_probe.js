'use strict';

/*
 * Read-only timing probe for candidate wasteful libroblox regions.
 *
 * Offsets are image-relative for the currently launched Roblox APK.  They
 * must be revalidated against the build-id after an APK update.  The probe
 * never changes arguments, return values, memory, scheduling, or Vulkan
 * state.  It hooks the stable host Vulkan exports and attributes their
 * callers to libroblox.  It deliberately does not patch stripped internal
 * PCs: sampled PCs can be basic-block entries rather than ABI-safe function
 * entries, and attaching there can crash the process.
 */
const targets = [
  { name: 'vkResetCommandPool', exportName: 'vkResetCommandPool' },
  { name: 'vkAcquireNextImageKHR', exportName: 'vkAcquireNextImageKHR' },
  { name: 'vkQueueSubmit', exportName: 'vkQueueSubmit' },
  { name: 'vkQueuePresentKHR', exportName: 'vkQueuePresentKHR' },
];
const counters = {};

const vulkan = Process.enumerateModules().find(m => m.name === 'libvulkan.so');
const robloxRange = Process.enumerateRanges('r-x').find(range =>
  range.file !== undefined && range.file.path.includes('libroblox.so'));
if (vulkan === undefined) {
  send({ error: 'host libvulkan.so is not loaded' });
} else {
  for (const target of targets) {
    const address = vulkan.findExportByName(target.exportName);
    if (address === null) {
      send({ missing_export: target.exportName });
      continue;
    }
    counters[target.name] = {
      address: address.toString(),
      calls: 0,
      totalUs: 0,
      maxUs: 0,
      over1ms: 0,
      over5ms: 0,
      over20ms: 0,
      robloxCalls: 0,
      otherCalls: 0,
      callers: {},
    };
    Interceptor.attach(address, {
      onEnter() {
        this.startUs = Date.now() * 1000;
        this.tid = Process.getCurrentThreadId();
        this.caller = this.returnAddress.toString();
        this.callerInRoblox = robloxRange !== undefined &&
          this.returnAddress.compare(robloxRange.base) >= 0 &&
          this.returnAddress.compare(robloxRange.base.add(robloxRange.size)) < 0;
      },
      onLeave() {
        const elapsedUs = Date.now() * 1000 - this.startUs;
        const counter = counters[target.name];
        counter.calls++;
        counter.totalUs += elapsedUs;
        counter.maxUs = Math.max(counter.maxUs, elapsedUs);
        if (elapsedUs >= 1000) counter.over1ms++;
        if (elapsedUs >= 5000) counter.over5ms++;
        if (elapsedUs >= 20000) counter.over20ms++;
        const caller = this.caller;
        if (this.callerInRoblox) {
          counter.robloxCalls++;
          counter.callers[caller] = (counter.callers[caller] || 0) + 1;
        } else {
          counter.otherCalls++;
        }
      },
    });
  }

  send({ hooked: Object.fromEntries(Object.entries(counters).map(([name, value]) =>
    [name, value.address])) });

  const report = () => {
    const snapshot = {};
    for (const [name, counter] of Object.entries(counters)) {
      const callers = Object.entries(counter.callers)
        .sort((a, b) => b[1] - a[1])
        .slice(0, 8)
        .map(([address, calls]) => ({ address, calls }));
      snapshot[name] = {
        calls: counter.calls,
        total_us: counter.totalUs,
        avg_us: counter.calls ? counter.totalUs / counter.calls : 0,
        max_us: counter.maxUs,
        over1ms: counter.over1ms,
        over5ms: counter.over5ms,
        over20ms: counter.over20ms,
        roblox_calls: counter.robloxCalls,
        other_calls: counter.otherCalls,
        top_callers: callers,
      };
      counter.calls = 0;
      counter.totalUs = 0;
      counter.maxUs = 0;
      counter.over1ms = 0;
      counter.over5ms = 0;
      counter.over20ms = 0;
      counter.robloxCalls = 0;
      counter.otherCalls = 0;
      counter.callers = {};
    }
    send({ interval_ms: 1000, counters: snapshot });
    setTimeout(report, 1000);
  };
  setTimeout(report, 1000);
}
