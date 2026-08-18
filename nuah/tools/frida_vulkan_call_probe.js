'use strict';

/* Read-only late-attach Vulkan call counter for a running Nuah process. */
const names = [
  'vkUpdateDescriptorSets',
  'vkAllocateDescriptorSets',
  'vkCmdBindDescriptorSets',
  'vkCmdCopyBufferToImage',
  'vkQueueSubmit',
  'vkQueueSubmit2'
];
const counters = {};

function attachExport(module, name) {
  let address;
  try {
    address = module.getExportByName(name);
  } catch (_) {
    return false;
  }
  const key = module.name + ':' + name;
  counters[key] = 0;
  Interceptor.attach(address, { onEnter() { ++counters[key]; } });
  send({kind: 'attached', name: key, address: address.toString()});
  return true;
}

const modules = Process.enumerateModules();
const vulkanModules = modules.filter(m => /libvulkan(\.so|_intel)/i.test(m.name));
send({kind: 'modules', modules: modules
  .filter(m => /vulkan|nuah/i.test(m.name))
  .map(m => ({name: m.name, base: m.base.toString(), path: m.path}))});
for (const module of vulkanModules) {
  for (const name of names) attachExport(module, name);
}
setInterval(function () {
  send({kind: 'vulkan_calls', calls: counters});
  for (const name of Object.keys(counters)) counters[name] = 0;
}, 1000);
