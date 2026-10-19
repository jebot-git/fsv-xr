#!/usr/bin/env python3
"""Set DomeD's file-manager defaults without changing other desktop sessions."""
import argparse
import configparser
import os
from pathlib import Path
import tempfile


def configure(directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "dome-mimeapps.list"
    config = configparser.ConfigParser(interpolation=None, delimiters=("=",), strict=False)
    config.optionxform = str
    if path.exists():
        with path.open(encoding="utf-8") as source:
            config.read_file(source)
    if not config.has_section("Default Applications"):
        config.add_section("Default Applications")
    for mime in ("inode/directory", "x-scheme-handler/trash"):
        config.set("Default Applications", mime, "org.fsvxr.fsv-xr.desktop;")
    fd, temporary = tempfile.mkstemp(prefix=".dome-mimeapps-", dir=directory)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            config.write(output, space_around_delimiters=False)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return path


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config-home", default=os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config")))
    args = parser.parse_args()
    print(configure(args.config_home))
