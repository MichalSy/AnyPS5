import argparse
import os
import struct
import sys
from pathlib import Path


def check_elf(path, types):
    if not path.is_file():
        raise ValueError(f"File does not exist: {path}")
    with path.open("rb") as stream:
        header = stream.read(20)
    if (len(header) != 20 or header[:7] != b"\x7fELF\x02\x01\x01" or
            header[7] not in (0, 3) or struct.unpack_from("<HH", header, 16)[0] not in types or
            struct.unpack_from("<HH", header, 16)[1] != 62):
        raise ValueError(f"Expected a native Linux x86-64 ELF file: {path}")


def launch(runtime, game, executable, workdir, arguments):
    if sys.platform != "linux":
        raise ValueError("Shared-runtime launching is supported on Linux only")
    runtime = runtime.expanduser().resolve()
    game = game.expanduser().resolve()
    executable = executable.expanduser().resolve()
    workdir = workdir.expanduser().resolve() if workdir is not None else executable.parent
    if not runtime.is_dir():
        raise ValueError(f"Runtime directory does not exist: {runtime}")
    if ":" in str(runtime) or "$" in str(runtime):
        raise ValueError("Runtime paths cannot contain ':' or '$' in the loader search path")
    for name in ("libc.prx", "libkernel.prx"):
        check_elf(runtime / name, (3,))
    if not game.is_dir():
        raise ValueError(f"Original game directory does not exist: {game}")
    check_elf(executable, (2, 3))
    if not os.access(executable, os.X_OK):
        raise ValueError(f"Game executable is not executable: {executable}")
    if not workdir.is_dir():
        raise ValueError(f"Working directory does not exist: {workdir}")
    environment = dict(os.environ)
    inherited = environment.get("LD_LIBRARY_PATH")
    environment["LD_LIBRARY_PATH"] = str(runtime) + (os.pathsep + inherited if inherited else "")
    environment["ANYPS5_GAME_ROOT"] = str(game)
    environment["ANYPS5_CODE_ROOT"] = str(executable.parent)
    os.chdir(workdir)
    os.execve(executable, [str(executable), *arguments], environment)


def main():
    parser = argparse.ArgumentParser(description="Start a prepared native game using shared AnyPS5 libraries")
    parser.add_argument("--runtime", required=True, type=Path,
                        help="Directory containing the complete compatible patched native PRX libraries")
    parser.add_argument("--game", required=True, type=Path, help="Original game directory containing its resources")
    parser.add_argument("--executable", required=True, type=Path, help="Generated native Linux game executable")
    parser.add_argument("--workdir", type=Path, help="Writable runtime directory; defaults to the executable's directory")
    parser.add_argument("arguments", nargs=argparse.REMAINDER, help="Game arguments after --")
    args = parser.parse_args()
    arguments = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
    try:
        launch(args.runtime, args.game, args.executable, args.workdir, arguments)
    except ValueError as error:
        parser.error(str(error))
    except OSError as error:
        parser.exit(1, f"{parser.prog}: {error}\n")


if __name__ == "__main__":
    main()
