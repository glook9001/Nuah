#pragma once

/* Android's stable native-handle ABI, required by libhybris common. */
typedef struct native_handle {
  int version;
  int numFds;
  int numInts;
  int data[0];
} native_handle_t;

native_handle_t* native_handle_create(int numFds, int numInts);
int native_handle_delete(native_handle_t* handle);
int native_handle_close(const native_handle_t* handle);
native_handle_t* native_handle_clone(const native_handle_t* handle);
