#!/usr/bin/env python3
"""Build-time only: freeze the shared transport and its dependencies for shipping.

Run with the target platform's interpreter after installing ./remote and
PyInstaller. Never downloads or installs anything on an end user's machine.
"""
import argparse
import importlib.metadata
import json
import platform
import shutil
import subprocess
import sys
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
output = args.output.resolve()
work = output.parent / 'remote-freeze-work'
work.mkdir(parents=True, exist_ok=True)
entry = work / 'entry.py'
entry.write_text('from pvt_remote.host import main\nif __name__ == "__main__":\n    main()\n')
if sys.platform == 'darwin':
    if int(platform.mac_ver()[0].split('.')[0]) < 27:
        raise SystemExit('PVT requires macOS 27.0 or later')
    native = work / 'pvt-videotoolbox.dylib'
    sdk = Path(subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
    subprocess.run(['xcrun', 'clang++', '-std=c++17', '-O2', '-dynamiclib',
        '-fobjc-arc', '-mmacosx-version-min=27.0', '-nostdlib++',
        str(sdk / 'usr/lib/libc++.tbd'),
        str(root / 'remote/pvt_remote/macos_video.mm'), '-o', str(native),
        '-framework', 'Foundation', '-framework', 'VideoToolbox',
        '-framework', 'CoreMedia', '-framework', 'CoreVideo'], check=True)

subprocess.run([sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean', '--onedir',
    '--name', 'pvt-remote', '--distpath', str(output.parent), '--workpath', str(work / 'build'),
    '--specpath', str(work), '--paths', str(root / 'remote'), '--collect-all', 'av',
    '--collect-all', 'zeroconf', '--collect-all', 'aiortc', str(entry)], check=True)
bundle = output.parent / 'pvt-remote'
if bundle != output:
    if output.exists():
        raise SystemExit(f'Refusing to overwrite {output}')
    bundle.rename(output)
if sys.platform == 'darwin':
    # This library links only Apple system frameworks and libc++. Preserve the
    # current Apple linker's layout; PyInstaller's Mach-O rewriting is unnecessary.
    shutil.copy2(native, output / '_internal' / native.name)
# Preserve the complete installed dependency license metadata with the worker.
licenses = output / 'licenses'
licenses.mkdir(exist_ok=True)
versions = {}
for distribution in importlib.metadata.distributions():
    name = distribution.metadata['Name']
    versions[name] = distribution.version
    for item in distribution.files or []:
        if any(word in str(item).lower() for word in ('license', 'copying', 'notice')):
            source = Path(distribution.locate_file(item))
            if source.is_file():
                destination = licenses / name / str(item).replace('..', '_')
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, destination)
(licenses / 'versions.json').write_text(json.dumps(versions, indent=2) + '\n')
executable = output / ('pvt-remote.exe' if sys.platform == 'win32' else 'pvt-remote')
subprocess.run([str(executable), '--self-test'], check=True, timeout=45)
