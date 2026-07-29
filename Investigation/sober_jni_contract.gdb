# Load after setting $nuah_sober_base and NUAH_JNI_TRACE.  This is deliberately
# version-pinned to the Sober build documented in README.md / the existing JNI
# table capture.  It records the JNI *contract*; it does not patch Sober or
# Roblox memory.

python
import gdb
import os
import struct

TRACE_PATH = os.environ.get("NUAH_JNI_TRACE")
if not TRACE_PATH:
    raise gdb.GdbError("set NUAH_JNI_TRACE before sourcing sober_jni_contract.gdb")

trace = open(TRACE_PATH, "a", encoding="utf-8", buffering=1)
inferior = gdb.selected_inferior()

def clean(value):
    return value.replace("\\", "\\\\").replace("\t", "\\t").replace("\n", "\\n")

def c_string(address):
    if not address:
        return "<null>"
    try:
        raw = inferior.read_memory(address, 512).tobytes()
        return clean(raw.split(b"\0", 1)[0].decode("utf-8", "replace"))
    except gdb.MemoryError:
        return "<unreadable>"

def reg(name):
    return int(gdb.parse_and_eval("$" + name))

def emit(*fields):
    trace.write("\t".join(str(field) for field in fields) + "\n")

class FindClassReturn(gdb.FinishBreakpoint):
    def __init__(self, frame, name):
        super().__init__(frame, internal=True)
        self.name = name
        self.silent = True
    def stop(self):
        try:
            emit("class", hex(int(self.return_value)), self.name)
        except gdb.error:
            emit("class", "0x0", self.name)
        return False

class FindClass(gdb.Breakpoint):
    def stop(self):
        FindClassReturn(gdb.newest_frame(), c_string(reg("rsi")))
        return False

class Lookup(gdb.Breakpoint):
    def __init__(self, address, kind):
        super().__init__("*" + hex(address), internal=True)
        self.kind = kind
        self.silent = True
    def stop(self):
        emit("lookup", self.kind, hex(reg("rsi")), c_string(reg("rdx")),
             c_string(reg("rcx")))
        return False

class RegisterNatives(gdb.Breakpoint):
    def stop(self):
        clazz, methods, count = reg("rsi"), reg("rdx"), reg("rcx")
        if count < 0 or count > 4096:
            emit("invalid-register", hex(clazz), hex(methods), count)
            return False
        for index in range(count):
            try:
                raw = inferior.read_memory(methods + index * 24, 24).tobytes()
                name, signature, _function = struct.unpack("QQQ", raw)
                emit("register", hex(clazz), c_string(name), c_string(signature))
            except (gdb.MemoryError, struct.error):
                emit("invalid-register", hex(clazz), hex(methods), index)
                break
        return False

base = int(gdb.parse_and_eval("$nuah_sober_base"))
FindClass("*" + hex(base + 0x4d0890), internal=True).silent = True
Lookup(base + 0x4d0f00, "method")
Lookup(base + 0x4d1a00, "field")
Lookup(base + 0x4d26e0, "static-method")
Lookup(base + 0x4d2e10, "static-field")
RegisterNatives("*" + hex(base + 0x4d5490), internal=True).silent = True
gdb.write("Nuah JNI contract recorder attached; output: %s\\n" % TRACE_PATH)
end
