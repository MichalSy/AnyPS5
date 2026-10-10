import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


def main():
    compiler = sys.argv[1] if len(sys.argv) > 1 else "c++"
    launcher = Path(__file__).resolve().parents[1] / "run_game.py"
    with tempfile.TemporaryDirectory(prefix="anyps5-launch-") as directory:
        root = Path(directory)
        runtime = root / "selected runtime"
        game = root / "prepared game"
        inherited = root / "inherited libraries"
        alternate = root / "alternate workdir"
        module = game / "app0/sce_module/title.guest.prx"
        for path in (runtime, game / "libs", inherited, alternate, module.parent):
            path.mkdir(parents=True)

        def compile_image(destination, source, *options):
            result = subprocess.run([compiler, "-x", "c++", "-std=c++20", "-", "-o", str(destination), *options],
                                    input=source, text=True, capture_output=True, timeout=10)
            assert result.returncode == 0, result.stderr

        for path, value in ((runtime, 17), (game / "libs", 99), (inherited, 88)):
            compile_image(path / "libc.prx", 'extern "C" int RuntimeValue() { return ' + str(value) + '; }',
                          "-shared", "-fPIC", "-Wl,-soname,libc.prx")
            compile_image(path / "libkernel.prx",
                          'extern "C" int RuntimeValue(); extern "C" int KernelValue() { return RuntimeValue() + 1; }',
                          "-shared", "-fPIC", "-Wl,-soname,libkernel.prx", "-L" + str(path),
                          "-l:libc.prx", "-Wl,-rpath,$ORIGIN")
        compile_image(inherited / "libinherited.so", 'extern "C" int InheritedValue() { return 7; }',
                      "-shared", "-fPIC", "-Wl,-soname,libinherited.so")
        compile_image(module, 'extern "C" int KernelValue(); extern "C" int TitleValue() { return KernelValue() + 1; }',
                      "-shared", "-fPIC", "-Wl,-soname,$ORIGIN/app0/sce_module/title.guest.prx",
                      "-L" + str(game / "libs"), "-l:libkernel.prx", "-Wl,-rpath,$ORIGIN/../../libs")
        executable = game / "game.elf"
        compile_image(executable, '#include <filesystem>\n#include <iostream>\n' + r'''
extern "C" int KernelValue();
extern "C" int TitleValue();
extern "C" int InheritedValue();
int main(int argc, char* argv[]) {
    std::cout << std::filesystem::current_path().string() << '\n';
    std::cout << KernelValue() << ' ' << TitleValue() << ' ' << InheritedValue() << '\n';
    for (int index = 0; index < argc; ++index) std::cout << argv[index] << '\n';
    return 37;
}
''', "-L" + str(game / "libs"), "-l:libkernel.prx", "-L" + str(inherited),
                      "-l:libinherited.so", "-x", "none", str(module), "-Wl,-rpath,$ORIGIN/libs")
        environment = dict(os.environ, LD_LIBRARY_PATH=str(inherited))
        guest = ["argument with spaces", "--runtime", "literal game path", "$(touch forbidden); *", "", "--"]

        def run(game_path=executable, runtime_path=runtime, workdir=None, arguments=()):
            command = [sys.executable, "-B", str(launcher), "--runtime", str(runtime_path), "--game", str(game_path)]
            if workdir is not None:
                command += ["--workdir", str(workdir)]
            return subprocess.run([*command, "--", *arguments], env=environment, text=True,
                                  capture_output=True, timeout=5, cwd=root)

        for workdir in (None, alternate):
            result = run(workdir=workdir, arguments=guest)
            expected = [str(workdir or game), "18 19 7", str(executable), *guest]
            assert result.returncode == 37, (result.returncode, result.stderr)
            assert result.stdout == "\n".join(expected) + "\n", repr(result.stdout)

        invalid = root / "invalid.elf"
        invalid.write_bytes(b"\x4f\x15\x3d\x1d" + bytes(100))
        invalid.chmod(0o755)
        assert run(game_path=invalid).returncode == 2
        image = bytearray(executable.read_bytes())
        struct.pack_into("<H", image, 18, 183)
        invalid.write_bytes(image)
        assert run(game_path=invalid).returncode == 2
        empty = root / "empty runtime"
        empty.mkdir()
        for game_path, runtime_path, workdir in ((root / "missing.elf", runtime, None),
                                                (game, runtime, None),
                                                (executable, root / "missing runtime", None),
                                                (executable, empty, None),
                                                (executable, runtime, root / "missing workdir")):
            result = run(game_path, runtime_path, workdir)
            assert result.returncode == 2 and result.stderr, (result.returncode, result.stderr)
        executable.chmod(0o644)
        assert run().returncode == 2
        executable.chmod(0o755)
        image = bytearray(executable.read_bytes())
        offset = struct.unpack_from("<Q", image, 32)[0]
        size, count = struct.unpack_from("<HH", image, 54)
        for index in range(count):
            header = offset + index * size
            if struct.unpack_from("<I", image, header)[0] == 3:
                start = struct.unpack_from("<Q", image, header + 8)[0]
                length = struct.unpack_from("<Q", image, header + 32)[0] - 1
                image[start:start + length] = b"/" + b"z" * (length - 1)
                break
        else:
            raise AssertionError("Native fixture has no ELF interpreter")
        invalid.write_bytes(image)
        assert run(game_path=invalid).returncode == 1
    print("Native game launch integration check passed")


if __name__ == "__main__":
    main()
