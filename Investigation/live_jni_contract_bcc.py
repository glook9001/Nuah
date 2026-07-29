#!/usr/bin/env python3
"""Record Sober's JNI contract without becoming its ptrace tracer.

Usage:
    sudo python3 Investigation/live_jni_contract_bcc.py <sober-client-pid> OUTPUT.tsv

The Sober controller already ptraces its client.  BCC uprobes are independent
of that relationship.  `binary` is resolved through /proc/<pid>/root by BCC,
which makes /app/bin/sober the correct in-sandbox path even when its executable
pages have been copied to memfd.
"""
from __future__ import annotations

import ctypes as ct
import sys
from pathlib import Path

from bcc import BPF


if len(sys.argv) != 3 or not sys.argv[1].isdigit():
    raise SystemExit(f"usage: {sys.argv[0]} SOBER_CLIENT_PID OUTPUT.tsv")

pid = int(sys.argv[1])
output = Path(sys.argv[2])
if output.exists():
    raise SystemExit(f"refusing to overwrite {output}")

# These are Sober image VMAs, recovered by live_jni_table_resolve for Flatpak
# revision 3f0141ef9c95ff47a08a1437d5b328bd8a6cdf749de592b06579dd5f3fcf948b.
FIND_CLASS = 0x4D0890
GET_METHOD_ID = 0x4D0F00
GET_FIELD_ID = 0x4D1A00
GET_STATIC_METHOD_ID = 0x4D26E0
GET_STATIC_FIELD_ID = 0x4D2E10
REGISTER_NATIVES = 0x4D5490
binary = "/app/bin/sober"

program = r"""
#include <uapi/linux/ptrace.h>

#define NAME_LEN 192
#define SIG_LEN 256
#define KIND_CLASS 1
#define KIND_METHOD 2
#define KIND_FIELD 3
#define KIND_STATIC_METHOD 4
#define KIND_STATIC_FIELD 5
#define KIND_REGISTER 6

struct event_t {
  u32 pid;
  u32 tid;
  u32 kind;
  u64 clazz;
  char name[NAME_LEN];
  char signature[SIG_LEN];
};

struct find_t { char name[NAME_LEN]; };
struct native_method_t { u64 name; u64 signature; u64 function; };

BPF_PERF_OUTPUT(events);
BPF_HASH(find_names, u32, struct find_t);
BPF_PERCPU_ARRAY(scratch, struct event_t, 1);

static inline int submit_lookup(struct pt_regs *ctx, u32 kind) {
  int zero = 0;
  struct event_t *event = scratch.lookup(&zero);
  if (!event) return 0;
  __builtin_memset(event, 0, sizeof(*event));
  event->pid = bpf_get_current_pid_tgid() >> 32;
  event->tid = (u32)bpf_get_current_pid_tgid();
  event->kind = kind;
  event->clazz = PT_REGS_PARM2(ctx);
  bpf_probe_read_user_str(&event->name, sizeof(event->name),
                          (void *)PT_REGS_PARM3(ctx));
  bpf_probe_read_user_str(&event->signature, sizeof(event->signature),
                          (void *)PT_REGS_PARM4(ctx));
  events.perf_submit(ctx, event, sizeof(*event));
  return 0;
}

int find_class(struct pt_regs *ctx) {
  u32 tid = (u32)bpf_get_current_pid_tgid();
  struct find_t value = {};
  bpf_probe_read_user_str(&value.name, sizeof(value.name),
                          (void *)PT_REGS_PARM2(ctx));
  find_names.update(&tid, &value);
  return 0;
}

int find_class_return(struct pt_regs *ctx) {
  u32 tid = (u32)bpf_get_current_pid_tgid();
  struct find_t *value = find_names.lookup(&tid);
  if (!value) return 0;
  int zero = 0;
  struct event_t *event = scratch.lookup(&zero);
  if (!event) return 0;
  __builtin_memset(event, 0, sizeof(*event));
  event->pid = bpf_get_current_pid_tgid() >> 32;
  event->tid = tid;
  event->kind = KIND_CLASS;
  event->clazz = PT_REGS_RC(ctx);
  __builtin_memcpy(&event->name, &value->name, sizeof(event->name));
  events.perf_submit(ctx, event, sizeof(*event));
  find_names.delete(&tid);
  return 0;
}

int get_method_id(struct pt_regs *ctx) { return submit_lookup(ctx, KIND_METHOD); }
int get_field_id(struct pt_regs *ctx) { return submit_lookup(ctx, KIND_FIELD); }
int get_static_method_id(struct pt_regs *ctx) { return submit_lookup(ctx, KIND_STATIC_METHOD); }
int get_static_field_id(struct pt_regs *ctx) { return submit_lookup(ctx, KIND_STATIC_FIELD); }

int register_natives(struct pt_regs *ctx) {
  const u64 clazz = PT_REGS_PARM2(ctx);
  const u64 methods = PT_REGS_PARM3(ctx);
  const s64 count = PT_REGS_PARM4(ctx);
  if (count < 0 || count > 64) return 0;
#pragma unroll
  for (int index = 0; index < 64; ++index) {
    if (index >= count) break;
    struct native_method_t method = {};
    int zero = 0;
    struct event_t *event = scratch.lookup(&zero);
    if (!event) return 0;
    __builtin_memset(event, 0, sizeof(*event));
    bpf_probe_read_user(&method, sizeof(method),
                        (void *)(methods + index * sizeof(method)));
    event->pid = bpf_get_current_pid_tgid() >> 32;
    event->tid = (u32)bpf_get_current_pid_tgid();
    event->kind = KIND_REGISTER;
    event->clazz = clazz;
    bpf_probe_read_user_str(&event->name, sizeof(event->name),
                            (void *)method.name);
    bpf_probe_read_user_str(&event->signature, sizeof(event->signature),
                            (void *)method.signature);
    events.perf_submit(ctx, event, sizeof(*event));
  }
  return 0;
}
"""


class Event(ct.Structure):
    _fields_ = [
        ("pid", ct.c_uint32), ("tid", ct.c_uint32), ("kind", ct.c_uint32),
        ("clazz", ct.c_uint64), ("name", ct.c_char * 192),
        ("signature", ct.c_char * 256),
    ]


def text(value: bytes) -> str:
    return value.split(b"\0", 1)[0].decode("utf-8", "replace").replace("\t", "\\t")


kind_names = {1: "class", 2: "method", 3: "field", 4: "static-method",
              5: "static-field", 6: "register"}


with output.open("x", encoding="utf-8", buffering=1) as stream:
    stream.write(f"# sober-jni-live-v1 pid={pid} backend=bcc\n")

    def show_event(_, data, __):
        event = ct.cast(data, ct.POINTER(Event)).contents
        name, signature = text(bytes(event.name)), text(bytes(event.signature))
        if event.kind == 1:
            line = f"class\t0x{event.clazz:x}\t{name}"
        elif event.kind == 6:
            line = f"register\t0x{event.clazz:x}\t{name}\t{signature}"
        else:
            line = f"lookup\t{kind_names[event.kind]}\t0x{event.clazz:x}\t{name}\t{signature}"
        stream.write(line + "\n")
        print(line, flush=True)

    bpf = BPF(text=program)
    bpf.attach_uprobe(name=binary, addr=FIND_CLASS, fn_name="find_class", pid=pid)
    bpf.attach_uretprobe(name=binary, addr=FIND_CLASS, fn_name="find_class_return", pid=pid)
    bpf.attach_uprobe(name=binary, addr=GET_METHOD_ID, fn_name="get_method_id", pid=pid)
    bpf.attach_uprobe(name=binary, addr=GET_FIELD_ID, fn_name="get_field_id", pid=pid)
    bpf.attach_uprobe(name=binary, addr=GET_STATIC_METHOD_ID, fn_name="get_static_method_id", pid=pid)
    bpf.attach_uprobe(name=binary, addr=GET_STATIC_FIELD_ID, fn_name="get_static_field_id", pid=pid)
    bpf.attach_uprobe(name=binary, addr=REGISTER_NATIVES, fn_name="register_natives", pid=pid)
    bpf["events"].open_perf_buffer(show_event)
    print(f"attached to PID {pid}; exercise Sober then Ctrl-C to finish", flush=True)
    try:
        while True:
            bpf.perf_buffer_poll()
    except KeyboardInterrupt:
        pass
