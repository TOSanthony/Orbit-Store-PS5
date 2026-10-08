"""Prepare the public catalogue feed. Does not commit, push, build an ELF, or fetch URLs."""
import argparse
import json
from pathlib import Path
from catalog import ROOT, compile_catalog, read, now
from catalog_crypto import FEED, unseal, source_envelopes, sealed_bytes


def compatible_rows(rows):
    """The v3 clients reject the entire feed if it contains an unknown format."""
    return [row for row in rows if row['format'] in ('FFPFSC', 'exFAT')]


def prepare(public_repo):
    public_repo = Path(public_repo).resolve()
    if not (public_repo / '.git').exists():
        raise ValueError('Expected an existing checkout of the public project repository')
    games = read('games.json')
    for game in games:
        if not game.get('addedAt'):
            game['addedAt'] = now()
    source_releases = read('releases.json')
    rows = compile_catalog(games, source_releases)
    target = public_repo / 'catalogue-v4.enc'
    compatible = public_repo / 'catalogue-v3.enc'
    older_rows = compatible_rows(rows)
    if not older_rows:
        raise ValueError('The compatibility feed needs at least one supported option')
    legacy = [public_repo / name for name in ('catalogue.json', 'catalogue-v2.json')]
    revision_path = ROOT / 'catalog/revision.json'
    revision = json.loads(revision_path.read_text())['revision']
    if type(revision) is not int or not 1 <= revision < 2147483647:
        raise ValueError('Invalid local catalogue revision')
    changed = False
    for current in [*legacy, compatible, target]:
        if current.exists():
            previous = json.loads(unseal(current.read_bytes(), FEED) if current.suffix == '.enc' else current.read_bytes())
            if previous.get('schemaVersion') != 1 or type(previous.get('revision')) is not int or not 1 <= previous['revision'] < 2147483647 or not isinstance(previous.get('releases'), list):
                raise ValueError(f'Existing public feed {current.name} is invalid; inspect it before publishing')
            revision = max(revision, previous['revision'])
            releases = ([{k: v for k, v in row.items() if k != 'browserUrl'}
                         for row in older_rows if row['sourceId'] == 'archive']
                        if current.name == 'catalogue.json' else
                        rows if current == target else older_rows)
            changed |= releases != previous['releases']
    if changed:
        revision += 1
    if revision >= 2147483647:
        raise ValueError('Catalogue revision limit reached')
    writes = source_envelopes(ROOT / 'catalog', games, source_releases, rows, revision)
    # The public feed and bundled fallback are byte-for-byte the same envelope.
    encoded = next(data for path, data in writes if path.name == 'catalog.enc')
    writes.append((target, encoded))
    writes.append((compatible, sealed_bytes(
        {'schemaVersion': 1, 'revision': revision, 'releases': older_rows}, FEED, compatible)))
    # All validation finishes before writing. Atomic replacements leave reviewable files.
    for path, data in [*writes,
                       (ROOT / 'catalog/games.json', (json.dumps(games, ensure_ascii=False, indent=2) + '\n').encode()),
                       (ROOT / 'catalog/catalog.json', (json.dumps(rows, ensure_ascii=False, indent=2) + '\n').encode()),
                       (revision_path, (json.dumps({'revision': revision}) + '\n').encode())]:
        temporary = path.with_suffix('.publish-tmp')
        temporary.write_bytes(data)
        temporary.replace(path)
    # Older clients retain their bundled/cached catalogue. Keeping these files
    # public would also keep a plaintext copy of the encrypted feed available.
    for path in legacy:
        path.unlink(missing_ok=True)
    return revision, len({row['gameId'] for row in rows}), len(rows)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--public-repo', required=True, type=Path)
    args = parser.parse_args()
    revision, games, options = prepare(args.public_repo)
    print(f'Prepared catalogue revision {revision}: {games} games, {options} validated single-file options.')
    print('Prepared catalogue-v4.enc and the format-compatible catalogue-v3.enc locally. Review before publishing with the FPKG-capable app. Nothing was pushed.')
