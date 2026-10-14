"""Find a complete native build for the FrameRift shared-runtime launcher."""

import os
import re
from pathlib import Path


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
