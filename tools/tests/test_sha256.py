import ctypes
import hashlib
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class ShaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("cc")
        if not cc:
            raise unittest.SkipTest("native C compiler required; run this suite under WSL/Linux")
        cls.tmp = tempfile.TemporaryDirectory()
        root = Path(__file__).resolve().parents[1]
        lib = Path(cls.tmp.name)/"sha.so"
        subprocess.run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        str(root/"runtime/pilot_sha256.c"), "-o", str(lib)], check=True)
        cls.lib = ctypes.CDLL(str(lib))
        cls.lib.pilot_sha256_bytes.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
        cls.lib.pilot_sha256_file.argtypes = [ctypes.c_char_p, ctypes.c_uint64, ctypes.c_void_p]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_padding_and_large_input_against_hashlib(self):
        for size in (0, 1, 3, 55, 56, 63, 64, 65, 8191, 8192, 8193, 1000000):
            data = (b"abc0123456789" * ((size//13)+1))[:size]
            output = ctypes.create_string_buffer(32)
            self.lib.pilot_sha256_bytes(data, len(data), output)
            self.assertEqual(output.raw, hashlib.sha256(data).digest(), size)

    def test_file_size_and_streaming(self):
        path = Path(self.tmp.name)/"input.bin"
        data = b"hello"*5000
        path.write_bytes(data)
        output = ctypes.create_string_buffer(32)
        self.assertEqual(self.lib.pilot_sha256_file(str(path).encode(), len(data), output), 0)
        self.assertEqual(output.raw, hashlib.sha256(data).digest())
        for size in (0, len(data)-1, len(data)+1):
            self.assertNotEqual(self.lib.pilot_sha256_file(str(path).encode(), size, output), 0)


if __name__ == "__main__":
    unittest.main()
