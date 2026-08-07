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
// Returns the single host window used by the MVP when ART gives libandroid a
// different local-reference token for the same Java Surface object.  The
// returned window carries one acquired reference, just like from_surface.
NuahNativeWindow* nuah_native_window_default(void);
void nuah_native_window_set_default(NuahNativeWindow* window);
// Android EGL consumes the host-native window handle, not Nuah's registry
// object.  The handle is reference-counted through the same window object.
void* nuah_native_window_egl_handle(const NuahNativeWindow* window);
void nuah_native_window_set_egl_handle(NuahNativeWindow* window,
                                       void* egl_window);
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
