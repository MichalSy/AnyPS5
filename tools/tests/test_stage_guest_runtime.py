from pathlib import Path
import contextlib
import io
import sys
import tempfile
import unittest

PROJECT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(PROJECT / "tools"))
sys.path.insert(0, str(PROJECT / "core/relinker/relinker/tests"))
import stage_guest_runtime
from test_guest_host_libc import importing
from test_guest_intel_trampolines import PLAIN_SITE, guest_fixture
from test_guest_module_directories import needed_libraries


class StagingTests(unittest.TestCase):
    def test_explicit_module_directory_converts_title_module_and_preserves_inputs(self):
        with tempfile.TemporaryDirectory(prefix="anyps5-stage-runtime-") as directory:
            root = Path(directory)
            game = root / "game"
            source = game / "source"
            sdk = source / "sce_module"
            sdk.mkdir(parents=True)
            (sdk / "sdk.prx").write_bytes(guest_fixture(PLAIN_SITE))
            bundled = source / "prx"
            bundled.mkdir()
            (bundled / "bundled.prx").write_bytes(guest_fixture(PLAIN_SITE))
            modules = source / "Media/Modules"
            modules.mkdir(parents=True)
            engine = modules / "engine.prx"
            image = guest_fixture(PLAIN_SITE)
            engine.write_bytes(image)
            (modules / "engine.prx.guest.prx").write_bytes(b"stale converted output")
            (modules / "nested").mkdir()
            (modules / "nested/ignored.prx").write_bytes(image)
            executable = source / "eboot.elf"
            original = importing("engine.prx", "libkernel.prx")
            executable.write_bytes(original)
            build = root / "build"
            libraries = build / "core/libs/libs"
            libraries.mkdir(parents=True)
            for name in ("libc.prx", "libkernel.prx"):
                (libraries / name).write_bytes(b"local " + name.encode())
            (build / "CMakeCache.txt").write_text("CMAKE_HOME_DIRECTORY:INTERNAL=" + str(PROJECT) + "\n")
            (build / "build.ninja").write_text("build core/libs/libs: phony core/libs/libs/libc.prx core/libs/libs/libkernel.prx\n")
            (build / "core/libs/nid_patcher").symlink_to(RELINKER)
            relinker = build / "core/relinker/relinker"
            relinker.parent.mkdir(parents=True)
            relinker.symlink_to(RELINKER)
            with contextlib.redirect_stderr(io.StringIO()):
                stage_guest_runtime.prepare(game, "game.elf", build, module_directories=["Media/Modules"])
            runtime = game / "runtime"
            needed = needed_libraries((runtime / "game.elf").read_bytes())
            self.assertIn("$ORIGIN/app0/prx/engine.prx.guest.prx", needed)
            self.assertNotIn("engine.prx", needed)
            self.assertIn("libkernel.prx", needed)
            self.assertEqual({path.relative_to(runtime / "app0").as_posix()
                              for path in (runtime / "app0").rglob("*.guest.prx")},
                             {"sce_module/sdk.prx.guest.prx", "prx/bundled.prx.guest.prx", "prx/engine.prx.guest.prx"})
            self.assertTrue((runtime / "game.engine.prx.guest.prx.registry.json").is_file())
            self.assertEqual(engine.read_bytes(), image)
            self.assertEqual(executable.read_bytes(), original)
            self.assertTrue((runtime / "game.elf").stat().st_mode & 0o111)

    def test_duplicate_module_names_and_unsafe_directories_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            sdk = source / "sce_module"
            sdk.mkdir(parents=True)
            modules = source / "Media/Modules"
            modules.mkdir(parents=True)
            (sdk / "same.prx").write_bytes(guest_fixture(PLAIN_SITE))
            (modules / "same.prx").write_bytes(guest_fixture(PLAIN_SITE))
            with self.assertRaisesRegex(ValueError, "Duplicate module filename"):
                stage_guest_runtime.stage_module_parent(source, root / "duplicate", ["Media/Modules"])
            with self.assertRaisesRegex(ValueError, "Repeated module directory"):
                stage_guest_runtime.stage_module_parent(source, root / "repeated", ["Media/Modules", modules])
            with self.assertRaisesRegex(ValueError, "inside the source"):
                stage_guest_runtime.stage_module_parent(source, root / "outside", [".."])
            with self.assertRaisesRegex(ValueError, "inside the source"):
                stage_guest_runtime.stage_module_parent(source, root / "missing", ["missing"])
            self.assertFalse((root / "repeated").exists())
            self.assertFalse((root / "outside").exists())


if __name__ == "__main__":
    RELINKER = Path(sys.argv.pop(1)).resolve()
    unittest.main()
