#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NuahNativeWindow NuahNativeWindow;

// The registry is shared between Nuah's runtime and its separately loaded
// Android libandroid provider. A Java Surface handle is the stable lookup key.
NuahNativeWindow* nuah_native_window_register_surface(
    void* surface,
    void* host_window,
    int width,
    int height);
// Associates a Java Surface façade handle with an existing native window.
// The alias owns one reference until it is unregistered.
int nuah_native_window_alias_surface(NuahNativeWindow* window, void* surface);
void nuah_native_window_unregister_surface(void* surface);
NuahNativeWindow* nuah_native_window_from_surface(void* surface);
void nuah_native_window_acquire(NuahNativeWindow* window);
void nuah_native_window_release(NuahNativeWindow* window);
int nuah_native_window_width(const NuahNativeWindow* window);
int nuah_native_window_height(const NuahNativeWindow* window);
void* nuah_native_window_host(const NuahNativeWindow* window);
void nuah_native_window_update_geometry(
    NuahNativeWindow* window,
    int width,
    int height);

#ifdef __cplusplus
}
#endif
