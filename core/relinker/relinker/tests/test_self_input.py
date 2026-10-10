"""Convert plaintext SELF inputs in place into a separate generated-only directory."""

from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from test_guest_intel_trampolines import PLAIN_SITE, guest_fixture, main_fixture
from test_guest_module_directories import needed_libraries


def self_fixture(image):
    phoff, = struct.unpack_from("<Q", image, 32)
    phsize, phcount = struct.unpack_from("<HH", image, 54)
    headers = [struct.unpack_from("<IIQQQQQQ", image, phoff + index * phsize)
               for index in range(phcount)]
    payloads = [(index, header) for index, header in enumerate(headers) if header[0] == 1]
    payloads.reverse()
    count = len(payloads) + 1
    elf_offset = 32 + count * 32
    header_size = elf_offset + phoff + phsize * phcount
    result = bytearray(header_size + 16)
    struct.pack_into("<IBBBBIHHQHHI", result, 0,
                     0x1D3D154F, 0, 1, 1, 0x12, 0x101, header_size, 16, 0, count, 0x22, 0)
    result[elf_offset:header_size] = image[:phoff + phsize * phcount]
    struct.pack_into("<QQQQ", result, 32, 1 << 16, len(result), 0, 0)
    for entry, (index, header) in enumerate(payloads, 1):
        offset, size = header[2], header[5]
        struct.pack_into("<QQQQ", result, 32 + entry * 32,
                         (index << 20) | (1 << 11) | 4, len(result), size, size)
        result.extend(image[offset:offset + size])
    struct.pack_into("<Q", result, 16, len(result))
    return result


def main():
    relinker = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="anyps5-self-") as directory:
        work = Path(directory)
        memory_limit = {}
        if sys.platform == "linux":
            import resource
            memory_limit["preexec_fn"] = lambda: resource.setrlimit(resource.RLIMIT_AS, (256 * 1024 * 1024,) * 2)
        game = work / "original"
        modules = game / "sce_module"
        modules.mkdir(parents=True)
        executable = main_fixture()
        struct.pack_into("<IIQQQQQQ", executable, 64 + 2 * 56,
                         0x6FFFFF01, 0, 0x5000, 0, 0, 0x10, 0x10, 1)
        struct.pack_into("<IIQQQQQQ", executable, 64 + 3 * 56,
                         4, 0, 0x5010, 0, 0, 0x10, 0, 4)
        original = {game / "eboot.bin": self_fixture(executable)}
        original.update({modules / f"module{index}.prx": self_fixture(guest_fixture(PLAIN_SITE))
                         for index in range(3)})
        module = guest_fixture(PLAIN_SITE)
        struct.pack_into("<H", module, 56, 5)
        struct.pack_into("<IIQQQQQQ", module, 64 + 3 * 56,
                         0x6FFFFF01, 0, 0x7FFFFFC0, 0, 0, 0x10, 0x10, 1)
        struct.pack_into("<IIQQQQQQ", module, 64 + 4 * 56,
                         4, 0, 0x7FFFFFD0, 0, 0, 0x10, 0, 4)
        original[modules / "module2.prx"] = self_fixture(module)
        for path, contents in original.items():
            path.write_bytes(contents)
        output_dir = work / "generated"
        output_dir.mkdir()
        output = output_dir / "game.elf"
        result = subprocess.run([str(relinker), "--registry", str(game / "eboot.bin"), str(output)],
                                capture_output=True, text=True, timeout=20, **memory_limit)
        assert result.returncode == 0, (result.stdout, result.stderr)
        expected = {Path("game.elf"), Path("game.registry.json")}
        expected.update(Path(f"app0/sce_module/module{index}.prx.guest.prx") for index in range(3))
        expected.update(Path(f"game.module{index}.prx.guest.prx.registry.json") for index in range(3))
        generated = {path.relative_to(output_dir) for path in output_dir.rglob("*") if path.is_file()}
        assert generated == expected, generated
        assert not any(path.is_symlink() for path in output_dir.rglob("*")), output_dir
        assert {path for path in game.rglob("*") if path.is_file()} == set(original), game
        assert all(path.read_bytes() == contents for path, contents in original.items()), "Original input changed"
        assert output.read_bytes().startswith(b"\x7fELF"), output
        needed = needed_libraries(output.read_bytes())
        assert needed == [f"$ORIGIN/app0/sce_module/module{index}.prx.guest.prx" for index in range(3)], needed
        for index in range(3):
            artifact = output_dir / f"app0/sce_module/module{index}.prx.guest.prx"
            assert artifact.read_bytes().startswith(b"\x7fELF") and artifact.stat().st_size < 0x10000, artifact

        struct.pack_into("<Q", executable, 64 + 2 * 56 + 8, 0x7FFFFFC0)
        struct.pack_into("<Q", executable, 64 + 3 * 56 + 8, 0x7FFFFFD0)
        source = work / "distant-metadata.self"
        source.write_bytes(self_fixture(executable))
        target = work / "distant-metadata.elf"
        result = subprocess.run([str(relinker), "--skip-sce-module", str(source), str(target)],
                                capture_output=True, text=True, timeout=20, **memory_limit)
        assert result.returncode == 0 and target.stat().st_size < 0x10000, (result.stdout, result.stderr)

        malformed = []
        for flag, message in ((2, "Encrypted SELF"), (8, "Compressed SELF")):
            image = self_fixture(main_fixture())
            properties, = struct.unpack_from("<Q", image, 64)
            struct.pack_into("<Q", image, 64, properties | flag)
            malformed.append((image, message))
        image = self_fixture(main_fixture())
        struct.pack_into("<Q", image, 72, len(image) + 1)
        malformed.append((image, "SELF range exceeds file bounds"))
        image = self_fixture(main_fixture())
        struct.pack_into("<Q", image, 64, (0xFFFF << 20) | (1 << 11))
        malformed.append((image, "Invalid SELF program segment index"))
        image = self_fixture(main_fixture())
        struct.pack_into("<Q", image, 64, 4)
        malformed.append((image, "missing required program segment data"))
        malformed.append((self_fixture(main_fixture())[:-1], "SELF range exceeds file bounds"))
        for index, (contents, message) in enumerate(malformed):
            source = work / f"invalid{index}.self"
            target = work / f"invalid{index}.elf"
            source.write_bytes(contents)
            result = subprocess.run([str(relinker), "--skip-sce-module", str(source), str(target)],
                                    capture_output=True, text=True, timeout=20)
            assert result.returncode == 2 and message in result.stderr and not target.exists(), result.stderr
    print("Plaintext SELF conversion integration test passed")


if __name__ == "__main__":
    main()
