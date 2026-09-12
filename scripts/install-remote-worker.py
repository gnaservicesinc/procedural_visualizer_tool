#!/usr/bin/env python3
"""Install the optional PVT transport in its own user-owned Python environment.

Does not enable networking, change desktop settings, or modify system Python.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import venv

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--destination', type=Path, default=Path.home() / '.local/share/pvt-remotes/venv')
args = parser.parse_args()
if sys.version_info < (3, 11):
    raise SystemExit('Python 3.11 or newer is required.')
source = Path(__file__).resolve().parents[1] / 'remote'
if not (source / 'pyproject.toml').exists():
    raise SystemExit('Run this installer from the PVT source tree or installed remote-worker source package.')
venv.EnvBuilder(with_pip=True).create(args.destination)
python = args.destination / ('Scripts/python.exe' if sys.platform == 'win32' else 'bin/python')
subprocess.run([str(python), '-m', 'pip', 'install', str(source)], check=True)
print(f'Installed. PVT detects the default location automatically. Python path: {python}')
print('Open Settings > Networking & Remotes to pair devices and enable networking.')
