'use strict';

/* Read-only frame-gap probe.  It hooks the host Vulkan present entry point,
 * emits only >50 ms inter-present gaps, and captures the caller stack at the
 * gap.  It does not inspect assets, game memory, or authentication data. */
const thresholdUs = 50000;
let previousEnterUs = 0;
let hitches = 0;

const vulkan = Process.enumerateModules().find(m => m.name === 'libvulkan.so');
if (vulkan === undefined) {
  send({ error: 'host libvulkan.so is not loaded' });
} else {
  const present = vulkan.findExportByName('vkQueuePresentKHR');
  if (present === null) {
    send({ error: 'vkQueuePresentKHR export not found', module: vulkan.path });
  } else {
    send({ hooked: 'vkQueuePresentKHR', address: present.toString(), threshold_us: thresholdUs });
    Interceptor.attach(present, {
      onEnter() {
        const nowUs = Number(Process.getCurrentThreadId()) === -1 ? 0 : Date.now() * 1000;
        if (previousEnterUs !== 0) {
          const gapUs = nowUs - previousEnterUs;
          if (gapUs >= thresholdUs) {
            hitches++;
            send({
              hitch: hitches,
              gap_us: gapUs,
              tid: Process.getCurrentThreadId(),
              backtrace: Thread.backtrace(this.context, Backtracer.ACCURATE)
                .map(DebugSymbol.fromAddress)
                .map(symbol => symbol.toString())
                .slice(0, 16)
            });
          }
        }
        previousEnterUs = nowUs;
      }
    });
  }
}
