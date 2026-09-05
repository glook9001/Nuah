#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "nuah/native_backend.hpp"
namespace {
const char* env_nonempty(const char* n) { const char* v = std::getenv(n); return v && *v ? v : nullptr; }
bool env_flag(const char* n) { const char* v = env_nonempty(n); return v && std::strcmp(v, "0") != 0; }
void reexec_with_bundled_pthread_bridge(char** argv) {
  const char* req = std::getenv("NUAH_USE_BUNDLED_PTHREAD_BRIDGE"); if (!req || !std::strcmp(req, "0")) return;
  if (auto* r = env_nonempty("NUAH_PTHREAD_BRIDGE_READY"); r && !std::strcmp(r, "1")) return;
  const auto exe = std::filesystem::canonical("/proc/self/exe");
  std::vector<std::filesystem::path> cands; if (auto* c = env_nonempty("NUAH_BIONIC_TRANSLATION_DIR")) cands.emplace_back(c);
  cands.emplace_back(exe.parent_path() / "bionic-translation"); cands.emplace_back(exe.parent_path() / "lib/nuah/bionic-translation");
  std::filesystem::path sel; for (auto& c : cands) if (std::filesystem::is_regular_file(c / "libpthread_bio.so.0")) { sel = c; break; }
  if (sel.empty()) { if (env_flag("NUAH_PERF_TRACE")) std::cerr << "nuah performance: bundled pthread bridge unavailable; using host fallback\n"; return; }
  std::string lp = sel.string(); if (auto* e = env_nonempty("LD_LIBRARY_PATH")) { lp += ':'; lp += e; }
  if (::setenv("LD_LIBRARY_PATH", lp.c_str(), 1) || ::setenv("NUAH_PTHREAD_BRIDGE_READY", "1", 1)) throw std::runtime_error("cannot select Nuah's pthread bridge");
  if (env_flag("NUAH_PERF_TRACE")) std::cerr << "nuah performance: pthread bridge=" << (sel / "libpthread_bio.so.0") << '\n';
  ::execv(exe.c_str(), argv); throw std::runtime_error("cannot restart Nuah with its pthread bridge");
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc >= 2 && !std::strcmp(argv[1], "native-run")) {
      nuah::NativeLaunchOptions o; static constexpr struct { const char* k; int id; } kOpts[] = {{"--apk", 0}, {"--split", 1}, {"--activity", 2}, {"--uri", 3}, {"--data", 4}, {"--width", 5}, {"--height", 6}};
      for (int i = 2; i < argc; ++i) {
        const std::string k = argv[i]; if (i + 1 >= argc) throw std::runtime_error("missing value for " + k); const std::string v = argv[++i]; int id = -1;
        for (auto& x : kOpts) if (k == x.k) id = x.id;
        if (id < 0) throw std::runtime_error("unknown native-run option: " + k);
        if (id == 0) o.apk = v; else if (id == 1) o.split_apks.emplace_back(v); else if (id == 2) o.activity = v;
        else if (id == 3) o.uri = v; else if (id == 4) o.data_directory = v;
        else { (id == 5 ? o.width : o.height) = std::stoi(v); o.dimensions_explicit = true; }
      }
      if (o.apk.empty()) throw std::runtime_error("usage: nuah native-run --apk <file.apk> [--activity <class>] [--split <config.apk>] [--uri <uri>] [--data <directory>] [--width <px>] [--height <px>]");
      nuah::apply_native_host_environment();
      if (auto* r = env_nonempty("NUAH_HOST_ENV_READY"); !r || std::strcmp(r, "1")) {
        if (::setenv("NUAH_HOST_ENV_READY", "1", 1)) throw std::runtime_error("cannot mark native host environment ready");
        ::execv(std::filesystem::canonical("/proc/self/exe").c_str(), argv); throw std::runtime_error("cannot restart native-run with host libraries");
      }
      reexec_with_bundled_pthread_bridge(argv); return nuah::run_native(o);
    }
    std::cerr << "usage: nuah native-run --apk <file.apk> [--split <config.apk>] [--uri <uri>] [--data <directory>] [--width <px>] [--height <px>]\n";
    return 2;
  } catch (const std::exception& e) { std::cerr << "nuah: " << e.what() << '\n'; return 1; }
}
