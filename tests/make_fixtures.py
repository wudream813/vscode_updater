"""Offline ZIP fixtures; all outputs stay under the explicitly supplied directory."""
import pathlib
import struct
import sys
import zipfile

root = pathlib.Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)


def archive(name, entries):
    with zipfile.ZipFile(root / name, "w", compression=zipfile.ZIP_STORED) as z:
        for entry, content in entries:
            z.writestr(entry, content)


archive("valid.zip", [("folder/hello.txt", b"hello"), ("empty.txt", b""), ("中文/说明.txt", "安全测试".encode())])
archive("traversal.zip", [("../escaped.txt", b"must never escape")])
archive("absolute.zip", [("C:/escaped.txt", b"must never escape")])
archive("case-collision.zip", [("same.txt", b"one"), ("SAME.txt", b"two")])
archive("conflict.zip", [("folder", b"file"), ("folder/child.txt", b"child")])
archive("data.zip", [("data/settings.json", b"{}")])
archive("reserved.zip", [("NUL.txt", b"bad")])
link = zipfile.ZipInfo("link")
link.create_system = 3
link.external_attr = 0o120777 << 16
archive("symlink.zip", [(link, b"../outside")])
archive("corrupt.zip", [("hello.txt", b"original-content")])
corrupt = root / "corrupt.zip"
data = bytearray(corrupt.read_bytes())
name_len, extra_len = struct.unpack_from("<HH", data, 26)
data[30 + name_len + extra_len] ^= 1  # change payload without updating CRC
corrupt.write_bytes(data)
(root / "truncated.zip").write_bytes(b"PK\x03\x04broken")
print(f"Created offline fixtures in {root}")
