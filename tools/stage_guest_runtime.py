import argparse
import fcntl
import filecmp
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
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


def build_library_names(build):
    manifests = ((build / "build.ninja", "build core/libs/libs: phony "),
                 (build / "core/libs/CMakeFiles/libs.dir/build.make", "core/libs/CMakeFiles/libs:"))
    for manifest, prefix in manifests:
        if not manifest.is_file():
            continue
        with manifest.open() as stream:
            names = {name for line in stream if line.startswith(prefix)
                     for name in re.findall(r"(?:^|\s)core/libs/libs/([^/\s]+\.prx)(?=\s|$)", line)}
        if names:
            return names
    return set()


def select_build(project, override=None):
    project = Path(project).resolve()
    override = override or os.environ.get("ANYPS5_BUILD_DIR")
    if override:
        candidate = Path(override).expanduser()
        candidates = [candidate if candidate.is_absolute() else project / candidate]
    else:
        candidates = [directory for directory in project.iterdir() if directory.is_dir()]
    builds = []
    incomplete = []
    for candidate in candidates:
        candidate = candidate.resolve()
        cache = candidate / "CMakeCache.txt"
        if not cache.is_file():
            continue
        settings = dict(line.split("=", 1) for line in cache.read_text().splitlines()
                        if "=" in line and not line.startswith(("#", "//")))
        source = settings.get("CMAKE_HOME_DIRECTORY:INTERNAL")
        if not source or Path(source).resolve() != project:
            continue
        relinker = candidate / "core/relinker/relinker"
        patcher = candidate / "core/libs/nid_patcher"
        libraries = candidate / "core/libs/libs"
        names = build_library_names(candidate)
        if not {"libc.prx", "libkernel.prx"}.issubset(names):
            continue
        missing = [name for name in sorted(names) if not any(path.is_file() and path.stat().st_size
                   for path in (libraries / name, libraries / "unpatched" / name))]
        if missing:
            incomplete.append(str(candidate) + ": missing " + ", ".join(missing))
            continue
        artifacts = [path for name in names for path in
                     (libraries / name, libraries / "unpatched" / name) if path.is_file()]
        if not all(path.is_file() and os.access(path, os.X_OK) for path in (relinker, patcher)):
            continue
        artifacts += [relinker, patcher]
        builds.append((max(path.stat().st_mtime_ns for path in artifacts), str(candidate), candidate))
    if not builds:
        details = "; " + "; ".join(incomplete) if incomplete else ""
        if override:
            raise RuntimeError("Build directory must contain a complete local CMake build of the relinker and libs targets: " + str(candidates[0]) + details)
        raise RuntimeError("Build the relinker and PRX libraries in a local CMake build before preparing a game" + details)
    return max(builds)[2]


def stage_libraries(build, staging):
    libraries = build / "core/libs/libs"
    patcher = build / "core/libs/nid_patcher"
    libc = libraries / "unpatched/libc.prx"
    names = build_library_names(build)
    prepared = []
    for name in sorted(names):
        patched = libraries / name
        unpatched = libraries / "unpatched" / name
        dependencies = [patcher]
        if unpatched.is_file():
            dependencies.append(unpatched)
            if name != "libc.prx":
                if not libc.is_file():
                    raise RuntimeError("Missing unpatched libc export reference: " + str(libc))
                dependencies.append(libc)
        if unpatched.is_file() and (not patched.is_file() or
                any(path.stat().st_mtime_ns > patched.stat().st_mtime_ns for path in dependencies)):
            output = staging / "libs" / name
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(unpatched, output)
            arguments = [str(patcher), name.removesuffix(".prx")]
            if name != "libc.prx":
                arguments += ["--preserve-exports", str(libc)]
            subprocess.run(arguments + [str(output)], check=True)
            prepared.append(output)
        else:
            prepared.append(patched)
    return prepared


def stage_module_parent(source, staging, module_directories=()):
    source = Path(source).resolve()
    if not module_directories:
        return source
    directories = []
    for directory in module_directories:
        directory = Path(directory)
        directory = (directory if directory.is_absolute() else source / directory).resolve()
        if not directory.is_relative_to(source) or not directory.is_dir():
            raise ValueError("Additional module directory must exist inside the source directory: " + str(directory))
        if directory in directories or directory in [(source / name).resolve() for name in ("sce_module", "sce_modules", "prx")]:
            raise ValueError("Repeated module directory: " + str(directory))
        directories.append(directory)
    staging = Path(staging)
    staging.mkdir(parents=True)
    names = {}
    for name in ("sce_module", "sce_modules", "prx"):
        directory = source / name
        if not directory.exists():
            continue
        if not directory.is_dir():
            raise ValueError("Module path is not a directory: " + str(directory))
        for path in sorted(directory.iterdir()):
            if path.is_file() and not path.name.endswith(".guest.prx"):
                if path.name in names:
                    raise ValueError("Duplicate module filename: " + str(names[path.name]) + " and " + str(path))
                names[path.name] = path
        if name == "prx":
            directories.insert(0, directory)
        else:
            (staging / name).symlink_to(directory, target_is_directory=True)
    prx = staging / "prx"
    prx.mkdir()
    for directory in directories:
        for path in sorted(directory.iterdir()):
            if not path.is_file() or path.name.endswith(".guest.prx"):
                continue
            if path.name in names and names[path.name] != path:
                raise ValueError("Duplicate module filename: " + str(names[path.name]) + " and " + str(path))
            names[path.name] = path
            (prx / path.name).symlink_to(path)
    return staging


def prepare(game, executable, build=None, excluded_modules=(), module_directories=()):
    project = Path(__file__).resolve().parents[1]
    game = Path(game).resolve()
    if Path(executable).name != executable or not executable.endswith(".elf"):
        raise ValueError("Expected an executable filename ending in .elf")
    runtime = game / "runtime"
    build = select_build(project, build)
    print("Preparing runtime from " + str(build), file=sys.stderr)
    runtime.mkdir(parents=True, exist_ok=True)
    (runtime / "download0").mkdir(exist_ok=True)
    with (runtime / ".prepare.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        with tempfile.TemporaryDirectory(prefix=".prepare-", dir=runtime) as directory:
            staging = Path(directory)
            output = staging / executable
            libraries = stage_libraries(build, staging)
            modules = stage_module_parent(game / "source", staging / "modules", module_directories)
            arguments = [str(build / "core/relinker/relinker"), "--registry", "--to-intel",
                         "--sce-module-path", str(modules)]
            for module in excluded_modules:
                arguments += ["--exclude-sce-module", module]
            subprocess.run(arguments + [str(game / "source/eboot.elf"), str(output)], check=True)
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
                environment = dict(os.environ, ANYPS5_BUILD_DIR=str(build))
                subprocess.run([sys.executable, "-B", str(plugins)], env=environment, check=True)
            for registry in sorted(staging.glob("*.registry.json")):
                atomic_copy(registry, runtime / registry.name)
            atomic_copy(output, runtime / executable)


def main():
    parser = argparse.ArgumentParser(description="Prepare a Linux game runtime without truncating loaded files")
    parser.add_argument("game", type=Path)
    parser.add_argument("executable")
    parser.add_argument("--build", type=Path)
    parser.add_argument("--exclude-sce-module", action="append", default=[])
    parser.add_argument("--module-dir", action="append", default=[], type=Path,
                        help="Additional module directory inside source, relative to source or absolute; repeat for multiple directories")
    args = parser.parse_args()
    prepare(args.game, args.executable, args.build, args.exclude_sce_module, args.module_dir)


if __name__ == "__main__":
    main()
