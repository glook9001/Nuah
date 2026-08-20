import sys, os, struct

# In ELF dynamic section:
# DT_NULL (0) terminates the dynamic section, so never replace internal entries with 0!
# DT_DEBUG (21) is a benign tag safely ignored by the dynamic linker on non-debug loads.
# DT_VERNEED = 0x6ffffffe
# DT_VERNEEDNUM = 0x6fffffff
# DT_VERSYM = 0x6ffffff0
# DT_VERDEF = 0x6ffffffc
# DT_VERDEFNUM = 0x6ffffffd

DT_DEBUG = 21

def remove_symbol_versions(filepath):
    try:
        with open(filepath, "rb") as f:
            data = bytearray(f.read())
    except Exception:
        return

    if len(data) < 64 or data[:4] != b'\x7fELF':
        return

    # Check 64-bit
    if data[4] != 2:
        return

    e_phoff = struct.unpack_from("<Q", data, 32)[0]
    e_phentsize = struct.unpack_from("<H", data, 54)[0]
    e_phnum = struct.unpack_from("<H", data, 56)[0]

    modified = False
    for i in range(e_phnum):
        ph_offset = e_phoff + i * e_phentsize
        p_type = struct.unpack_from("<I", data, ph_offset)[0]
        if p_type == 2:  # PT_DYNAMIC
            p_offset = struct.unpack_from("<Q", data, ph_offset + 8)[0]
            p_filesz = struct.unpack_from("<Q", data, ph_offset + 32)[0]

            num_entries = p_filesz // 16
            for j in range(num_entries):
                entry_off = p_offset + j * 16
                d_tag = struct.unpack_from("<Q", data, entry_off)[0]
                # Neutralize DT_VERSYM, DT_VERDEF, DT_VERDEFNUM, DT_VERNEED, and DT_VERNEEDNUM
                if d_tag in (0x6ffffff0, 0x6ffffffc, 0x6ffffffd, 0x6ffffffe, 0x6fffffff):
                    struct.pack_into("<Q", data, entry_off, DT_DEBUG)
                    struct.pack_into("<Q", data, entry_off + 8, 0)
                    modified = True
            break

    if modified:
        with open(filepath, "wb") as f:
            f.write(data)
        print(f"Neutralized symbol version checks in {filepath}")

if __name__ == "__main__":
    for path in sys.argv[1:]:
        if os.path.isfile(path):
            remove_symbol_versions(path)
