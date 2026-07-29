#include "nuah/nuah_jvm.h"

#include <stddef.h>
#include "jvm/jvm.h"

#include <stdlib.h>
#include <string.h>

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

void* nuah_jvm_find_registered_native(NuahJvm* jvm, const char* class_name,
                                      const char* method_name,
                                      const char* signature) {
  if (!jvm || !class_name || !method_name || !signature) return NULL;
  for (size_t index = 0;
       index < sizeof(jvm->core.methods) / sizeof(jvm->core.methods[0]);
       ++index) {
    const struct jvm_native_method* candidate = &jvm->core.methods[index];
    if (!candidate->function) continue;
    const char* candidate_class =
        jvm_get_class_name(&jvm->core, candidate->method.klass);
    if (candidate_class && candidate->method.name.data &&
        candidate->method.signature.data &&
        strcmp(candidate_class, class_name) == 0 &&
        strcmp(candidate->method.name.data, method_name) == 0 &&
        strcmp(candidate->method.signature.data, signature) == 0) {
      return candidate->function;
    }
  }
  return NULL;
}
