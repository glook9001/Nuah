#include "nuah/jni_manifest.h"

#include <fstream>
#include <stdexcept>
#include <unordered_set>

namespace nuah {
namespace {

std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (true) {
    const auto end = line.find('\t', start);
    fields.push_back(line.substr(start, end - start));
    if (end == std::string::npos) return fields;
    start = end + 1;
  }
}

std::string entry_key(const JniManifestEntry& entry) {
  return entry.kind + "\n" + entry.class_name + "\n" + entry.member +
         "\n" + entry.signature;
}

}  // namespace

std::vector<JniManifestEntry> load_jni_manifest(const std::string& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot open JNI manifest: " + path);

  std::vector<JniManifestEntry> result;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (line.empty() || line.starts_with('#')) continue;
    const auto fields = split_tabs(line);
    if (fields.size() != 5) {
      throw std::runtime_error("invalid JNI manifest line " +
                               std::to_string(line_number));
    }
    result.push_back({fields[0], fields[1], fields[2], fields[3], fields[4]});
  }
  validate_jni_manifest(result);
  return result;
}

void validate_jni_manifest(const std::vector<JniManifestEntry>& entries) {
  if (entries.empty()) throw std::runtime_error("JNI manifest is empty");
  std::unordered_set<std::string> seen;
  for (const auto& entry : entries) {
    if ((entry.kind != "lookup" && entry.kind != "register") ||
        entry.class_name.empty() || entry.member.empty() ||
        entry.signature.empty() || entry.source.empty()) {
      throw std::runtime_error("JNI manifest has an incomplete entry");
    }
    if (!entry.signature.starts_with('(')) {
      throw std::runtime_error("JNI manifest has a non-JNI signature: " +
                               entry.member);
    }
    if (!seen.emplace(entry_key(entry)).second) {
      throw std::runtime_error("JNI manifest has a duplicate entry: " +
                               entry.member);
    }
  }
}

std::size_t jni_manifest_resolved_entries(
    const std::vector<JniManifestEntry>& entries) {
  std::size_t result = 0;
  for (const auto& entry : entries) {
    if (entry.class_name != "*") ++result;
  }
  return result;
}

}  // namespace nuah
