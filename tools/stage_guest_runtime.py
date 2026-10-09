import argparse
import fcntl
import filecmp
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def atomic_copy(source, destination):
    source = Path(source)
    destination = Path(destination)
    if destination.is_file() and filecmp.cmp(source, destination, shallow=False):
        return False
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix="." + destination.name + "-", dir=destination.parent)
    os.close(descriptor)
    temporary = Path(name)
    try:
        shutil.copy2(source, temporary)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)
    return True


def prepare(game, executable):
    project = Path(__file__).resolve().parents[1]
    game = Path(game).resolve()
    if Path(executable).name != executable or not executable.endswith(".elf"):
        raise ValueError("Expected an executable filename ending in .elf")
    runtime = game / "runtime"
    runtime.mkdir(parents=True, exist_ok=True)
    (runtime / "download0").mkdir(exist_ok=True)
    libraries = sorted((project / "build/core/libs/libs").glob("*.prx"))
    if not libraries:
        raise RuntimeError("Build the PRX libraries before preparing a game")
    for library in libraries:
        unpatched = library.parent / "unpatched" / library.name
        if unpatched.is_file() and unpatched.stat().st_mtime_ns > library.stat().st_mtime_ns:
            raise RuntimeError("Patched library is older than its linked build: " + library.name +
                               ". Run cmake --build build --target libs before preparing a game")
    with (runtime / ".prepare.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        with tempfile.TemporaryDirectory(prefix=".prepare-", dir=runtime) as directory:
            staging = Path(directory)
            output = staging / executable
            subprocess.run([str(project / "build/core/relinker/relinker"), "--registry", "--to-intel",
                            str(game / "source/eboot.elf"), str(output)], check=True)
            output.chmod(0o755)
            for library in libraries:
                atomic_copy(library, runtime / "libs" / library.name)
            for module in sorted((staging / "app0").rglob("*.guest.prx")):
                destination = runtime / module.relative_to(staging)
                if not destination.parent.resolve().is_relative_to(runtime.resolve()):
                    raise RuntimeError("Guest module destination escapes the runtime directory")
                atomic_copy(module, destination)
            plugins = game / "prepare_dynamic_plugins.py"
            if plugins.exists():
                subprocess.run(["python", "-B", str(plugins)], check=True)
            for registry in sorted(staging.glob("*.registry.json")):
                atomic_copy(registry, runtime / registry.name)
            atomic_copy(output, runtime / executable)


def main():
    parser = argparse.ArgumentParser(description="Prepare a Linux game runtime without truncating loaded files")
    parser.add_argument("game", type=Path)
    parser.add_argument("executable")
    args = parser.parse_args()
    prepare(args.game, args.executable)


if __name__ == "__main__":
    main()
