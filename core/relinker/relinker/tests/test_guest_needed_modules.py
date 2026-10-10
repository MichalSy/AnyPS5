from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile

from test_guest_intel_trampolines import PLAIN_SITE, guest_fixture, main_fixture
from test_guest_module_directories import module_with_symbol, needed_libraries


NEEDED = b"needed.prx"


def executable_with_needed(needed=NEEDED):
    image = main_fixture()
    strings = b"\0" + needed + b"\0"
    image[0x4800:0x4800 + len(strings)] = strings
    tags = []
    for position in range(0x4600, 0x4600 + 0x200, 16):
        tag, value = struct.unpack_from("<qQ", image, position)
        if tag == 0:
            break
        tags.append((tag, value))
    tags = [(5, 0x4800) if tag == 5 else (10, len(strings)) if tag == 10 else (tag, value) for tag, value in tags]
    tags += [(1, 1), (0, 0)]
    for index, tag in enumerate(tags):
        struct.pack_into("<qQ", image, 0x4600 + index * 16, *tag)
    struct.pack_into("<QQ", image, 120 + 32, len(tags) * 16, len(tags) * 16)
    return image


def module_with_needed(names, entries, soname=None):
    image = guest_fixture(PLAIN_SITE)
    strings = bytearray(b"\0")
    offsets = []
    for name in names:
        offsets.append(len(strings))
        strings += name + b"\0"
    if soname is not None:
        soname_offset = len(strings)
        strings += soname + b"\0"
    assert len(strings) <= 0x100
    image[0x900:0x900 + len(strings)] = strings
    tags = [(5, 0x2300), (10, len(strings)), (6, 0x2220), (11, 24),
            (4, 0x2240), (7, 0x2300), (8, 0), (9, 24)]
    tags += [(1, offsets[index]) for index in entries]
    if soname is not None:
        tags.append((14, soname_offset))
    tags.append((0, 0))
    for index, tag in enumerate(tags):
        struct.pack_into("<qQ", image, 0x600 + index * 16, *tag)
    struct.pack_into("<QQ", image, 176 + 32, len(tags) * 16, len(tags) * 16)
    return image


def main():
    relinker = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="anyps5-needed-modules-") as directory:
        work = Path(directory)

        def convert(case, windows, needed=NEEDED):
            (case / "sce_module").mkdir(parents=True, exist_ok=True)
            source = case / "input.elf"
            source.write_bytes(executable_with_needed(needed))
            output = case / ("output.exe" if windows else "output.elf")
            result = subprocess.run([str(relinker), *(["--windows"] if windows else []), str(source), str(output)],
                                    capture_output=True, text=True, timeout=30)
            return result, output

        for windows in (False, True):
            for label, dependency, filename, soname in (
                    ("filename-case", b"Party.prx", "party.prx", None),
                    ("soname-case", b"pArTy.prx", "provider.prx", b"Party.prx")):
                case = work / f"{windows}-{label}"
                (case / "prx").mkdir(parents=True)
                (case / "prx" / filename).write_bytes(module_with_needed((), (), soname))
                result, output = convert(case, windows, dependency)
                assert result.returncode == 0, (label, windows, result.stdout, result.stderr)
                artifact = case / "app0" / "prx" / (filename + ".guest.prx")
                assert list((case / "app0").rglob("*.guest.prx")) == [artifact], artifact
                assert artifact.read_bytes().startswith(b"MZ" if windows else b"\x7fELF"), artifact
                if windows and os.name == "nt":
                    run = subprocess.run([str(output)], capture_output=True, text=True, timeout=30)
                    assert run.returncode == 42, (run.returncode, run.stdout, run.stderr)
                if not windows:
                    assert needed_libraries(output.read_bytes()) == [f"$ORIGIN/app0/prx/{filename}.guest.prx"]
                    assert needed_libraries(artifact.read_bytes()) == []

            for label, dependency in (("case-ambiguous", b"FOO.prx"), ("case-exact", b"Foo.prx")):
                case = work / f"{windows}-{label}"
                (case / "prx").mkdir(parents=True)
                for filename, soname in (("first.prx", b"Foo.prx"), ("second.prx", b"foo.prx")):
                    (case / "prx" / filename).write_bytes(module_with_needed((), (), soname))
                if label == "case-exact":
                    (case / "prx" / "consumer.prx").write_bytes(module_with_needed((dependency,), (0,)))
                result, output = convert(case, windows, dependency)
                if label == "case-ambiguous":
                    assert result.returncode == 2 and "Ambiguous guest" in result.stderr, result.stderr
                    assert not output.exists() and not (case / "app0").exists(), output
                    continue
                assert result.returncode == 0, (windows, result.stdout, result.stderr)
                artifacts = list((case / "app0").rglob("*.guest.prx"))
                assert {artifact.name for artifact in artifacts} == {
                    "first.prx.guest.prx", "second.prx.guest.prx", "consumer.prx.guest.prx"}, artifacts
                if windows and os.name == "nt":
                    run = subprocess.run([str(output)], capture_output=True, text=True, timeout=30)
                    assert run.returncode == 42, (run.returncode, run.stdout, run.stderr)
                if not windows:
                    assert set(needed_libraries(output.read_bytes())) == {
                        f"$ORIGIN/app0/prx/{artifact.name}" for artifact in artifacts}
                    consumer = case / "app0" / "prx" / "consumer.prx.guest.prx"
                    assert needed_libraries(consumer.read_bytes()) == ["$ORIGIN/first.prx.guest.prx"]

            case = work / f"{windows}-found"
            modules = case / "Media" / "Modules"
            modules.mkdir(parents=True)
            (modules / "needed.prx").write_bytes(module_with_symbol(True))
            (case / "Media" / "needed.prx").write_text("not ELF")
            result, output = convert(case, windows)
            assert result.returncode == 0, (result.stdout, result.stderr)
            artifact = case / "app0" / "Media" / "Modules" / "needed.prx.guest.prx"
            assert artifact.read_bytes().startswith(b"MZ" if windows else b"\x7fELF"), artifact
            assert list((case / "app0").rglob("*.guest.prx")) == [artifact]
            assert "    Media/Modules/needed.prx.guest.prx\n" in result.stdout, result.stdout
            if windows and os.name == "nt":
                run = subprocess.run([str(output)], capture_output=True, text=True, timeout=30)
                assert run.returncode == 42, (run.returncode, run.stdout, run.stderr)
            if not windows:
                needed = needed_libraries(output.read_bytes())
                assert needed == ["$ORIGIN/app0/Media/Modules/needed.prx.guest.prx"], needed

            case = work / f"{windows}-ambiguous"
            for name in ("first", "second"):
                (case / name).mkdir(parents=True)
                (case / name / "needed.prx").write_bytes(module_with_symbol(True))
            result, output = convert(case, windows)
            assert result.returncode == 2 and "Ambiguous needed module" in result.stderr, result.stderr
            assert not output.exists(), output
    print("Guest needed module tests passed")


if __name__ == "__main__":
    main()
