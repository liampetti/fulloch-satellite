import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class TimezoneTests(unittest.TestCase):
    def test_timezone_mapping_and_seasonal_offsets(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = pathlib.Path(directory) / "test_timezone"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "main"), str(ROOT / "tests/test_timezone.c"),
                 "-o", str(executable)],
                check=True,
            )
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
