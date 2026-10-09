from pathlib import Path
import ctypes
import os
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

from test_guest_intel_trampolines import main_fixture


def guest_image(name, dependency=None):
    image = bytearray(0x4000)
    image[:16] = b"\x7fELF\x02\x01\x01" + bytes(9)
    struct.pack_into("<HHIQQQIHHHHHH", image, 16,
                     3, 62, 1, 0x1000, 64, 0, 0, 64, 56, 4, 0, 0, 0)
    struct.pack_into("<IIQQQQQQ", image, 64,
                     1, 5, 0, 0, 0, 0x2000, 0x2000, 0x1000)
    struct.pack_into("<IIQQQQQQ", image, 120,
                     1, 6, 0x2000, 0x2000, 0x2000, 0x2000, 0x2000, 0x1000)
    struct.pack_into("<IIQQQQQQ", image, 232,
                     0x6474E551, 6, 0, 0, 0, 0, 0, 16)
    strings = bytearray(b"\0")

    def string(value):
        offset = len(strings)
        strings.extend(value.encode() + b"\0")
        return offset

    symbol = string(name + "State#A#B")
    tags = [] if dependency is None else [(1, string(dependency + ".prx"))]
    tags += [(5, 0x2200), (10, len(strings)), (6, 0x2400), (11, 24),
             (4, 0x2600), (7, 0x2800), (8, 24), (9, 24),
             (12, 0x1000), (13, 0x1200), (25, 0x2A00), (27, 8), (0, 0)]
    struct.pack_into("<IIQQQQQQ", image, 176,
                     2, 6, 0x2000, 0x2000, 0x2000, len(tags) * 16, len(tags) * 16, 8)
    for index, entry in enumerate(tags):
        struct.pack_into("<qQ", image, 0x2000 + index * 16, *entry)
    image[0x2200:0x2200 + len(strings)] = strings
    struct.pack_into("<IBBHQQ", image, 0x2418, symbol, 0x12, 0, 1, 0x1300, 8)
    struct.pack_into("<IIIII", image, 0x2600, 1, 2, 1, 0, 0)
    struct.pack_into("<QQq", image, 0x2800, 0x2A00, 8, 0x1100)

    def code(address, emit):
        output = bytearray()

        def put(value):
            output.extend(bytes.fromhex(value))

        def rip(value, target):
            put(value)
            output.extend(struct.pack("<i", target - address - len(output) - 4))

        emit(put, rip)
        image[address:address + len(output)] = output

    def initialize(put, rip):
        rip("48 89 3d", 0x2C00)
        rip("48 89 35", 0x2C08)
        rip("48 89 15", 0x2C10)
        rip("ff 05", 0x2C18)
        put("48 85 f6 74 16 48 8b 46 08")
        rip("48 89 05", 0x2C28)
        put("48 83 ec 08 ff d0 48 83 c4 08 c3 b8 11 00 00 00 c3")

    def finalize(put, rip):
        rip("48 8b 05", 0x2C28)
        put("48 85 c0 74 0a 48 83 ec 08 ff d0 48 83 c4 08 c3")

    code(0x1000, initialize)
    code(0x1100, lambda put, rip: (rip("ff 05", 0x2C1C), put("c3")))
    code(0x1200, finalize)
    code(0x1300, lambda put, rip: (rip("48 8d 05", 0x2C00), put("c3")))
    return image


class State(ctypes.Structure):
    _fields_ = [("args", ctypes.c_size_t), ("argp", ctypes.c_void_p),
                ("reserved", ctypes.c_void_p), ("inits", ctypes.c_uint32),
                ("constructors", ctypes.c_uint32)]


class StartBlock(ctypes.Structure):
    _fields_ = [("size", ctypes.c_uint32), ("version", ctypes.c_uint32),
                ("callback", ctypes.c_void_p)]


def main():
    relinker = Path(sys.argv[1]).resolve()
    kernel_path = Path(sys.argv[2]).resolve()
    kernel = ctypes.CDLL(str(kernel_path), mode=os.RTLD_GLOBAL)
    kernel.sceKernelLoadStartModule.argtypes = [ctypes.c_char_p, ctypes.c_size_t,
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
    kernel.sceKernelLoadStartModule.restype = ctypes.c_int32
    kernel.dlopen_nid_postfix.argtypes = [ctypes.c_char_p, ctypes.c_int]
    kernel.dlopen_nid_postfix.restype = ctypes.c_void_p
    kernel.dlsym_nid_postfix.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    kernel.dlsym_nid_postfix.restype = ctypes.c_void_p
    kernel.dlclose_nid_postfix.argtypes = [ctypes.c_void_p]
    kernel.dlclose_nid_postfix.restype = ctypes.c_int
    kernel.dlerror_nid_postfix.restype = ctypes.c_char_p
    callback_type = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_size_t,
                                     ctypes.c_void_p, ctypes.c_void_p)
    getter_type = ctypes.CFUNCTYPE(ctypes.POINTER(State))
    callbacks = []
    events = []
    errors = []
    original_directory = Path.cwd()

    with tempfile.TemporaryDirectory(prefix="anyps5-module-start-") as directory:
        root = Path(directory)
        modules = root / "sce_module"
        modules.mkdir()
        names = ["Dependency", "Target", "Nested", "Plain", "AfterFailure", "ThreadA", "ThreadB"]
        for name in names:
            (modules / (name + ".prx")).write_bytes(
                guest_image(name, "Dependency" if name == "Target" else None))
        source = root / "input.elf"
        source.write_bytes(main_fixture())
        converted = subprocess.run([str(relinker), "--rpath", str(kernel_path.parent),
            str(source), str(root / "output.elf")], capture_output=True, text=True, timeout=30)
        assert converted.returncode == 0, (converted.stdout, converted.stderr)
        os.chdir(root)

        def path(name):
            return ("/app0/sce_module/" + name + ".prx").encode()

        def state(handle, name):
            pointer = kernel.dlsym_nid_postfix(handle, (name + "State").encode())
            assert pointer, kernel.dlerror_nid_postfix()
            return getter_type(pointer)().contents

        def close(handle):
            assert kernel.dlclose_nid_postfix(handle) == 0

        def block(name, result, nested=None):
            value = StartBlock(16, 0x200, None)

            @callback_type
            def observe(args, argp, reserved):
                try:
                    if argp is None:
                        assert args == 0 and reserved is None
                        events.append((name, "fini"))
                        return 0
                    assert args == 16 and argp == ctypes.addressof(value) and reserved is None
                    assert value.size == 16 and value.version == 0x200
                    events.append((name, "init"))
                    if nested is not None:
                        nested()
                    return result
                except BaseException as error:
                    errors.append(error)
                    return -1

            callbacks.append(observe)
            value.callback = ctypes.cast(observe, ctypes.c_void_p).value
            return value

        def load(name, value, expected):
            result = ctypes.c_int(999)
            handle = kernel.sceKernelLoadStartModule(path(name), 16,
                ctypes.byref(value), 0, None, ctypes.byref(result))
            assert handle > 0, (handle, kernel.dlerror_nid_postfix())
            assert result.value == expected, (name, result.value, expected)
            return handle

        nested_block = block("Nested", 23)
        plain_handle = None

        def nested_loads():
            nonlocal plain_handle
            child = load("Nested", nested_block, 23)
            assert state(child, "Nested").argp == ctypes.addressof(nested_block)
            close(child)
            plain_handle = kernel.dlopen_nid_postfix(path("Plain"), 2)
            assert plain_handle
            plain = state(plain_handle, "Plain")
            assert plain.args == 0 and plain.argp is None and plain.reserved is None

        target_block = block("Target", -37, nested_loads)
        target = load("Target", target_block, -37)
        target_state = state(target, "Target")
        assert target_state.args == 16 and target_state.argp == ctypes.addressof(target_block)
        assert target_state.inits == 1 and target_state.constructors == 1
        dependency = kernel.dlopen_nid_postfix(path("Dependency"), 2)
        assert dependency
        dependency_state = state(dependency, "Dependency")
        assert dependency_state.args == 0 and dependency_state.argp is None
        assert dependency_state.reserved is None
        assert dependency_state.inits == 1 and dependency_state.constructors == 1
        repeated = load("Target", target_block, 0)
        assert repeated != target and state(repeated, "Target").inits == 1
        sentinel_result = ctypes.c_int(999)
        sentinel = kernel.sceKernelLoadStartModule(path("Target"), ctypes.c_size_t(-1).value,
            None, 0, None, ctypes.byref(sentinel_result))
        assert sentinel > 0 and sentinel not in (target, repeated) and sentinel_result.value == 0
        assert state(sentinel, "Target").inits == 1 and state(sentinel, "Target").constructors == 1
        close(sentinel)
        close(target)
        assert events.count(("Target", "fini")) == 0
        close(repeated)
        assert events.count(("Target", "fini")) == 1
        close(dependency)
        close(plain_handle)
        target = load("Target", target_block, -37)
        assert state(target, "Target").inits == 1
        close(target)
        close(plain_handle)

        result = ctypes.c_int(999)
        assert kernel.sceKernelLoadStartModule(path("Missing"), 16,
            ctypes.byref(target_block), 0, None, ctypes.byref(result)) < 0
        assert result.value == 0
        kernel.dlerror_nid_postfix()
        fresh = kernel.sceKernelLoadStartModule(path("AfterFailure"), ctypes.c_size_t(-1).value,
            None, 0, None, ctypes.byref(result))
        assert fresh > 0 and result.value == 17
        assert state(fresh, "AfterFailure").args == ctypes.c_size_t(-1).value
        assert state(fresh, "AfterFailure").argp is None
        close(fresh)
        plain = kernel.dlopen_nid_postfix(path("AfterFailure"), 2)
        assert plain and state(plain, "AfterFailure").args == 0
        assert state(plain, "AfterFailure").argp is None
        preinitialized_block = block("AfterFailure", 29)
        preinitialized = load("AfterFailure", preinitialized_block, 0)
        assert preinitialized != plain and state(preinitialized, "AfterFailure").inits == 1
        assert state(preinitialized, "AfterFailure").argp is None
        close(preinitialized)
        close(plain)

        def worker(name, expected):
            value = block(name, expected)
            handle = load(name, value, expected)
            current = state(handle, name)
            assert current.argp == ctypes.addressof(value) and current.inits == 1
            close(handle)

        with ThreadPoolExecutor(max_workers=2) as workers:
            tasks = [workers.submit(worker, "ThreadA", 41), workers.submit(worker, "ThreadB", 42)]
            for task in tasks:
                task.result(timeout=20)
        assert not errors, errors
        assert events.count(("Target", "init")) == 2
        assert events.count(("Target", "fini")) == 2
        os.chdir(original_directory)
    print("Guest module start argument integration passed")


if __name__ == "__main__":
    main()
