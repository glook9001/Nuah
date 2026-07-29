#include "nuah/nuah_jvm.h"

#include <stddef.h>
#include "jvm/jvm.h"

#include <stdlib.h>

struct NuahJvm {
  struct jvm core;
};

NuahJvm* nuah_jvm_create(void) {
  NuahJvm* jvm = calloc(1, sizeof(*jvm));
  if (!jvm) return NULL;
  jvm_init(&jvm->core);
  return jvm;
}

void nuah_jvm_destroy(NuahJvm* jvm) {
  if (!jvm) return;
  jvm_release(&jvm->core);
  free(jvm);
}

void* nuah_jvm_java_vm(NuahJvm* jvm) {
  return jvm ? &jvm->core.vm : NULL;
}
