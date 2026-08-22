# Nuah Engineering Mindset & Performance Principles

This document captures the engineering philosophy, performance principles, and problem-solving behaviors that guide development in the Nuah runtime.

---

## 1. Core Philosophy: Zero-Overhead Hot Paths

In a high-performance translation and compatibility layer running real-time 3D workloads (such as Roblox games at 60+ FPS), **milliseconds are precious and microseconds are the currency of frame pacing**.

1. **Never allocate on hot paths**:
   - Zero heap allocations (`malloc`, `new`, `std::string`, `std::vector`) during rendering, input polling, math operations, or command recording.
   - Use stack buffers, fixed arrays, or cached reusable structures.
2. **Never query dynamic state repeatedly**:
   - Dynamic lookups like `::dlsym()`, `std::getenv()`, `JNIEnv::FindClass()`, `JNIEnv::GetMethodID()`, and `SDL_GetWindowSize()` must **never** execute on per-frame or per-poll loops.
   - Resolve once upon initialization or cache with `static const` / global references.
3. **Direct driver pass-through when tracing is off**:
   - Diagnostic tracing and telemetry must have zero impact on production performance.
   - When trace flags are inactive, wrappers and shims must immediately forward directly to the host driver / OS without timing, counting, or mutex acquisitions.

---

## 2. Key Engineering Behaviors

### 🔍 Root-Cause Diagnosis Over Surface Patching
- Do not stop at the symptom. Trace down to the exact failing layer:
  - Is a thread blocked in `fprintf` or JNI string conversion?
  - Is `_dl_load_lock` contending across worker threads due to repeated `dlsym`?
  - Are Wayland / SDL event pumps starving the compositor round-trip?
  - Are Vulkan descriptors being allocated individually instead of batched?
- Fix the fundamental architecture rather than adding workarounds.

### ⚡ Aggressive Static & Global Caching
- **Math Library (`libm.so`)**: Cache all mathematical function pointers statically so physics, particle engines, and skeletal transforms execute in 1 CPU cycle with 0 dynamic linker overhead.
- **JNI Dispatch (`real_art_jvm.cpp`)**: Cache `jclass` (as global refs) and `jmethodID` for frequently constructed events (`KeyEvent`, `MotionEvent`) to eliminate reflection overhead on every keystroke and mouse motion.
- **Input & Window Sessions**: Cache environment toggles (`NUAH_LOCK_SURFACE_SIZE`, `NUAH_NONBLOCK_WAYLAND_EVENTS`) and apply geometry updates to native windows only when dimensions actually change.

### 🛑 Gated Diagnostics & Logging
- Keep all Android runtime logging (`__android_log_print`, `nuah_art_log_println`) gated behind `NUAH_ANDROID_LOG`.
- In production, log printing and string formatting (`vsnprintf`) must be completely skipped, keeping JVM threads unblocked.

### 🧪 Empirical, Benchmark-Driven Validation
- Every modification must be validated in three stages:
  1. **Compilation**: Clean, zero-warning build with `ninja -C build`.
  2. **Automated Verification**: 100% test pass rate with `ctest --output-on-failure`.
  3. **Real-World Gameplay Benchmark**: Live testing in compute-heavy games (e.g. *Rivals* at 1280x720) measuring `p50`, `p95`, `p99` frametimes and ensuring 0 hitch frames over 33ms.

### 📦 Clean & Intentional Source Control
- Commit logical units of work with clear, descriptive commit messages explaining *what* was changed, *why* it was changed, and *what performance impact* was achieved.
- Maintain clean working trees and synchronize directly with the remote repository.
