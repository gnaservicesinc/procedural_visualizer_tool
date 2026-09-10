#!/usr/bin/env python3
"""Copy/check the explicitly maintained PVT tracker files (no network access)."""
import argparse
from pathlib import Path
import shutil

FILES = (
    "BTT.h", "PVTOnset.h", "LICENSE", "src/BTT.c", "src/PVTOnset.c",
    "src/DFT.c", "src/DFT.h", "src/Filter.c", "src/Filter.h", "src/STFT.c",
    "src/STFT.h", "src/Statistics.c", "src/Statistics.h", "src/fastsin.c",
    "src/fastsin.h", "tests/tracker_test.c",
)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Local PVT-Beat-and-Tempo-Tracking checkout")
    parser.add_argument("--check", action="store_true", help="Fail on differences; do not write")
    args = parser.parse_args()
    destination = Path(__file__).resolve().parents[1] / "third_party" / "btt"
    # Read every input first, so a partial/mistyped checkout cannot half-update PVT.
    source = args.source.resolve()
    for name in FILES:
        if not (source / name).is_file():
            parser.error(f"Missing tracker source: {source / name}")
    differences = [name for name in FILES if not (destination / name).is_file()
                   or (source / name).read_bytes() != (destination / name).read_bytes()]
    if args.check:
        if differences:
            print("Tracker differs: " + ", ".join(differences))
            return 1
        print(f"All {len(FILES)} tracker source/test files match.")
        return 0
    for name in differences:
        (destination / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / name, destination / name)
    print(f"Updated {len(differences)} tracker source/test files.")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
