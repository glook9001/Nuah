import sys

def patch_file(path, old_suffix=b"_77", new_suffix=b"_76"):
    with open(path, "rb") as f:
        data = f.read()
    count = data.count(b"ucnv_getToUCallBack" + old_suffix)
    if count:
        data = data.replace(b"ucnv_getToUCallBack" + old_suffix, b"ucnv_getToUCallBack" + new_suffix)
        with open(path, "wb") as f:
            f.write(data)
        print(f"Patched {count} instances in {path}")
    else:
        print(f"No match in {path}")

if __name__ == "__main__":
    patch_file(sys.argv[1])
