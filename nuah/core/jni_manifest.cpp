#include "nuah/jni_manifest.h"
#include <fstream>
#include <stdexcept>
#include <unordered_set>
namespace nuah {
static std::vector<std::string> split_tabs(const std::string& line) {
  std::vector<std::string> f;
  std::size_t s = 0;
  while (true) { const auto e = line.find('\t', s); f.push_back(line.substr(s, e - s)); if (e == std::string::npos) return f; s = e + 1; }
}
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
    if (fields.size() != 5) { throw std::runtime_error("invalid JNI manifest line " + std::to_string(line_number)); }
    result.push_back({fields[0], fields[1], fields[2], fields[3], fields[4]});
  }
  validate_jni_manifest(result);
  return result;
}
void validate_jni_manifest(const std::vector<JniManifestEntry>& entries) {
  if (entries.empty()) throw std::runtime_error("JNI manifest is empty");
  std::unordered_set<std::string> seen;
  for (const auto& e : entries) {
    if ((e.kind != "lookup" && e.kind != "register") || e.class_name.empty() || e.member.empty() || e.signature.empty() || e.source.empty()) throw std::runtime_error("JNI manifest has an incomplete entry");
    if (!e.signature.starts_with('(')) throw std::runtime_error("JNI manifest has a non-JNI signature: " + e.member);
    if (!seen.emplace(e.kind + "\n" + e.class_name + "\n" + e.member + "\n" + e.signature).second) throw std::runtime_error("JNI manifest has a duplicate entry: " + e.member);
  }
}
std::size_t jni_manifest_resolved_entries(const std::vector<JniManifestEntry>& entries) {
  std::size_t n = 0;
  for (const auto& e : entries) n += (e.class_name != "*");
  return n;
}
}  // namespace nuah
