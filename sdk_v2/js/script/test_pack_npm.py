import io
from pathlib import Path
import tarfile
import tempfile
import unittest

from pack_npm import REQUIRED_PAYLOADS, normalize_archive, validate_archive


class NpmArchiveTests(unittest.TestCase):
    def setUp(self):
        build = Path(__file__).resolve().parents[1] / "build"
        build.mkdir(exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=build)
        self.addCleanup(self.temporary.cleanup)
        self.archive = Path(self.temporary.name) / "package.tgz"

    def write_archive(self, omitted=None, directory=None):
        with tarfile.open(self.archive, "w:gz") as archive:
            for name in (*sorted(REQUIRED_PAYLOADS), "package/package.json", "package/dist/index.js"):
                if name == omitted:
                    continue
                entry = tarfile.TarInfo(name)
                entry.mode = 0o644
                entry.mtime = 123456789
                entry.uid, entry.gid = 123, 456
                entry.uname, entry.gname = "owner", "group"
                entry.pax_headers = {"comment": "preserve me"}
                payload = f"payload: {name}\n".encode()
                entry.size = len(payload)
                if name == directory:
                    entry.type = tarfile.DIRTYPE
                    entry.size = 0
                    archive.addfile(entry)
                else:
                    archive.addfile(entry, io.BytesIO(payload))
            entry = tarfile.TarInfo("package/dist")
            entry.type = tarfile.DIRTYPE
            entry.mode = 0o750
            archive.addfile(entry)
            entry = tarfile.TarInfo("package/dist/link")
            entry.type = tarfile.SYMTYPE
            entry.linkname = "index.js"
            entry.mode = 0o777
            archive.addfile(entry)

    def snapshot(self):
        with tarfile.open(self.archive, "r:gz") as archive:
            result = []
            for entry in archive:
                data = None
                if entry.isfile():
                    with archive.extractfile(entry) as payload:
                        data = payload.read()
                metadata = entry.get_info()
                # The tar header checksum necessarily changes when the mode changes.
                metadata.pop("chksum")
                result.append((metadata, entry.pax_headers, data))
            return result

    def test_normalizes_posix_hosts_without_changing_other_entries_or_payloads(self):
        self.write_archive()
        before = self.snapshot()
        normalize_archive(self.archive)
        after = self.snapshot()
        self.assertEqual(len(before), len(after))
        changed = []
        for (old, old_pax, old_data), (new, new_pax, new_data) in zip(before, after):
            if old["name"].endswith("/foundry-local-host"):
                changed.append(old["name"])
                self.assertEqual(new["mode"], 0o755)
                old["mode"] = 0o755
            self.assertEqual(new, old)
            self.assertEqual(new_pax, old_pax)
            self.assertEqual(new_data, old_data)
        self.assertEqual(len(changed), 3)
        validate_archive(self.archive)

    def test_rejects_missing_or_non_file_required_payloads_without_replacing_input(self):
        for name in REQUIRED_PAYLOADS:
            for kind in ("omitted", "directory"):
                with self.subTest(name=name, kind=kind):
                    self.write_archive(**{kind: name})
                    original = self.archive.read_bytes()
                    with self.assertRaisesRegex(ValueError, name):
                        normalize_archive(self.archive)
                    self.assertEqual(self.archive.read_bytes(), original)
                    self.assertEqual(list(self.archive.parent.iterdir()), [self.archive])

    def test_finished_archive_validation_rejects_non_executable_posix_hosts(self):
        self.write_archive()
        with self.assertRaisesRegex(ValueError, "not executable"):
            validate_archive(self.archive)


if __name__ == "__main__":
    unittest.main()
