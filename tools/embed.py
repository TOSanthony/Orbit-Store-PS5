"""Embed the built UI and curated catalogue; no filesystem web root on PS5."""
import json, mimetypes
from pathlib import Path
from catalog_crypto import unseal
root = Path(__file__).resolve().parent.parent
out = root / 'build/generated'
out.mkdir(parents=True, exist_ok=True)
parts = ['#ifndef ORBIT_ASSETS_H\n#define ORBIT_ASSETS_H\n#include <stddef.h>\n']
rows = []
for i, p in enumerate(sorted((root / 'dist').rglob('*'))):
    if not p.is_file(): continue
    data = p.read_bytes()
    parts.append(f'static const unsigned char asset_{i}[] = {{{",".join(str(b) for b in data)}}};\n')
    mime = mimetypes.guess_type(p.name)[0] or 'application/octet-stream'
    rows.append('{'+json.dumps('/'+str(p.relative_to(root/'dist')))+','+json.dumps(mime)+f',asset_{i},sizeof(asset_{i})'+'}')
parts.append('struct orbit_asset { const char *path, *mime; const unsigned char *data; size_t size; };\n')
parts.append('static const struct orbit_asset assets[] = {'+','.join(rows)+'};\n#endif\n')
(out/'assets.h').write_text(''.join(parts))
catalog = (root/'catalog/catalog.enc').read_bytes()
revision = json.loads((root/'catalog/revision.json').read_text())['revision']
if json.loads(unseal(catalog))['revision'] != revision:
    raise ValueError('Encrypted catalogue revision is stale; run catalog:build')
(out/'catalog.h').write_text(f'static const unsigned catalog_revision = {revision};\n' +
    'static const unsigned char catalog_data[] = {' + ','.join(str(b) for b in catalog) + '};\n')
ca = root/'.deps/cacert.pem'
if ca.exists():
    (out/'ca.h').write_text('static const char orbit_ca[] = '+json.dumps(ca.read_text())+';\n')
print(f'Embedded {len(rows)} UI assets and catalogue')
