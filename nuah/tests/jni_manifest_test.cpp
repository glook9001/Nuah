#include "nuah/jni_manifest.h"

#include <algorithm>
#include <iostream>

#ifndef NUAH_JNI_MANIFEST_PATH
#error "NUAH_JNI_MANIFEST_PATH must be configured by CMake"
#endif

int main() {
  const auto entries = nuah::load_jni_manifest(NUAH_JNI_MANIFEST_PATH);
  const auto resolved = nuah::jni_manifest_resolved_entries(entries);
  if (entries.size() != 29 || resolved != 25) {
    std::cerr << "unexpected JNI manifest coverage: total=" << entries.size()
              << " resolved=" << resolved << '\n';
    return 1;
  }
  const auto logging = std::find_if(
      entries.begin(), entries.end(), [](const nuah::JniManifestEntry& entry) {
        return entry.kind == "lookup" &&
               entry.class_name ==
                   "com/roblox/universalapp/logging/LoggingProtocol" &&
               entry.member == "getProcessTimestamp" && entry.signature == "()J";
      });
  if (logging == entries.end()) return 1;
  const auto key_down = std::find_if(
      entries.begin(), entries.end(), [](const nuah::JniManifestEntry& entry) {
        return entry.kind == "register" &&
               entry.class_name == "com/google/androidgamesdk/GameActivity" &&
               entry.member == "onKeyDownNative" &&
               entry.signature == "(JLandroid/view/KeyEvent;)Z";
      });
  return key_down == entries.end() ? 1 : 0;
}
