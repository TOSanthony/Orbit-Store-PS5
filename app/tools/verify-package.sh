#!/usr/bin/env bash
# Verify the packaged bytes, not just the staging folder. Run in the app build image.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
image=dist/PPSA99177.ffpkg
runner=$(bash tools/setup-packaging-dependencies.sh ffpkg)
mkdir -p build
temporary=$(mktemp -d "$root/build/verify-package.XXXXXX")
trap 'rm -rf -- "$temporary"' EXIT
"$runner" extract "$image" "$temporary" >"$temporary/extract.log"
python3 - "$temporary" <<'PY'
import hashlib, json, os, subprocess, sys
from pathlib import Path
extracted = Path(sys.argv[1])
def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()
payload = Path('../build/orbit_store.elf')
expected = digest(payload)
assert digest(extracted / 'orbit/orbit_store.elf') == expected, 'FFPKG carries a different payload'
assert digest(Path('dist/PPSA99177/orbit/orbit_store.elf')) == expected, 'Staged payload differs'
version = (extracted / 'orbit/version.txt').read_text().strip()
assert b'Orbit-Store/' + version.encode() + b'\0' in payload.read_bytes(), 'Payload version marker differs'
param = json.loads((extracted / 'sce_sys/param.json').read_text())
assert param == json.loads(Path('sce_sys/param.json').read_text()), 'Package metadata differs'
assert digest(extracted / 'eboot.bin') == digest(Path('dist/PPSA99177/eboot.bin')), 'App executable differs'
for language in Path('../i18n').glob('*.json'):
    if language.stem != 'en':
        assert digest(extracted / 'assets/i18n' / language.name) == digest(language), f'Packaged translation differs: {language.name}'
source = os.environ.get('ORBIT_SOURCE_COMMIT')
if not source and Path('../.git').exists():
    source = subprocess.check_output(['git', '-c', 'safe.directory=*', 'rev-parse', 'HEAD'], text=True).strip()
if not source and Path('../../SOURCES.json').is_file():
    source = json.loads(Path('../../SOURCES.json').read_text())['sourceCommit']
if not source:
    raise SystemExit('Set ORBIT_SOURCE_COMMIT when building outside a checkout or source bundle')
record = {'sourceCommit': source, 'payloadVersion': version, 'payloadSha256': expected,
          'imageSha256': digest(Path('dist/PPSA99177.ffpkg')),
          'appSha256': digest(extracted / 'eboot.bin'),
          'contentVersion': param['contentVersion'], 'titleId': param['titleId']}
Path('build/package-verification.json').write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record, indent=2))
PY
