# SPDX-License-Identifier: LGPL-2.1-or-later
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("dome_default", Path(__file__).parents[1] / "scripts/set-dome-default.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class DefaultsTest(unittest.TestCase):
    def test_dome_only_idempotent_and_preserves_other_preferences(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            global_config = root / "mimeapps.list"
            global_config.write_text("[Default Applications]\ninode/directory=org.kde.dolphin.desktop;\n")
            kde_config = root / "kde-mimeapps.list"
            kde_config.write_text(global_config.read_text())
            dome_config = root / "dome-mimeapps.list"
            dome_config.write_text("[Default Applications]\ntext/plain=org.kde.kate.desktop;\n")
            module.configure(root)
            first = dome_config.read_text()
            module.configure(root)
            self.assertEqual(dome_config.read_text(), first)
            self.assertIn("inode/directory=org.fsvxr.fsvr.desktop;", first)
            self.assertIn("x-scheme-handler/trash=org.fsvxr.fsvr.desktop;", first)
            self.assertIn("text/plain=org.kde.kate.desktop;", first)
            self.assertEqual(global_config.read_text(), kde_config.read_text())
            self.assertIn("dolphin", global_config.read_text())

if __name__ == "__main__":
    unittest.main()
