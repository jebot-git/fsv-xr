#!/usr/bin/env python3
"""Set DomeD's file-manager defaults without changing other desktop sessions."""
import argparse
import configparser
import os
from pathlib import Path
import tempfile
import re


def archive_types():
    prefix = Path(__file__).resolve().parents[1]
    candidates = (prefix / "data/archive-mime-types.list", prefix / "share/fsvr/archive-mime-types.list")
    for path in candidates:
        if path.is_file():
            return [line.strip() for line in path.read_text().splitlines() if line.strip() and not line.startswith("#")]
    raise FileNotFoundError("The installed archive MIME type list is missing")


def configure(directory, archives_only=False):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "dome-mimeapps.list"
    config = configparser.ConfigParser(interpolation=None, delimiters=("=",), strict=False)
    config.optionxform = str
    if path.exists():
        with path.open(encoding="utf-8") as source:
            config.read_file(source)
    types = archive_types()
    if not archives_only:
        types = ["inode/directory", "x-scheme-handler/trash", *types]
    # Validate with ConfigParser, then preserve comments, unrelated keys and
    # other sections verbatim while updating only the requested associations.
    lines = path.read_text().splitlines(keepends=True) if path.exists() else []
    if lines and not lines[-1].endswith("\n"):
        lines[-1] += "\n"
    found = set()
    output = []
    active = False
    section = False
    def missing():
        pending = [mime for mime in types if mime not in found]
        found.update(pending)
        return [mime + "=org.fsvxr.fsvr.desktop;\n" for mime in pending]
    for line in lines:
        match = re.match(r"\s*\[([^]]+)\]", line)
        if match:
            if active:
                output.extend(missing())
            active = match[1] == "Default Applications"
            section |= active
        key = line.split("=", 1)[0].strip() if "=" in line else None
        if active and key in types:
            if key not in found:
                output.append(key + "=org.fsvxr.fsvr.desktop;\n")
                found.add(key)
        else:
            output.append(line)
    if active:
        output.extend(missing())
    elif not section:
        output.extend(["[Default Applications]\n", *missing()])
    lines_to_write = output
    fd, temporary = tempfile.mkstemp(prefix=".dome-mimeapps-", dir=directory)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            output.write("".join(lines_to_write))
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return path


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config-home", default=os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config")))
    parser.add_argument("--archives-only", action="store_true", help="Change only archive handlers; preserve folder/trash choices")
    args = parser.parse_args()
    print(configure(args.config_home, args.archives_only))
