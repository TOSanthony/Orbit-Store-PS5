"""Stage reviewed single-file Vikingfile pages for Orbit's PS5 browser flow.

No networking or publishing. Preserve recorded check dates; metadata verification
is distinct from the bounded download validation performed on the console.
"""
import argparse
import copy
import hashlib
import json
import re
import unicodedata
from pathlib import Path
from urllib.parse import urlsplit
from catalog import ROOT, compile_catalog, https, now, viking_page, viking_metadata_evidence
from import_catalog import game_key


def artwork(entry, pippo, pegasus):
    tid = entry['title_id']
    candidates = []
    for p in pippo:
        if tid in p.get('tags', []) and https(p.get('image', '')):
            candidates.append((p['image'], 'https://pippo26442999.github.io/.exFAT/exFAT.json', p['title']))
    for p in pegasus:
        if p.get('titleId') == tid and https(p.get('posterUrl', '')):
            candidates.append((p['posterUrl'], p['_catalogSource'], p['title']))
    if not candidates:
        raise ValueError('No title-ID-matched external artwork')
    def rank(item):
        host = urlsplit(item[0]).hostname
        return (0 if host == 'cdn.prosperopatches.com' else
                2 if host and host.endswith('playstation.com') else 1,
                game_key(item[2]) != game_key(entry['game']))
    candidates.sort(key=rank)
    return candidates


def stage(entries, games, releases, metadata, pippo, pegasus, snapshot_hash):
    games, releases = copy.deepcopy(games), copy.deepcopy(releases)
    by_id = {g['id']: g for g in games}
    by_title = {game_key(g['title']): g for g in games}
    by_tid = {r['titleId']: by_id[r['gameId']] for r in releases}
    original_ids = set(by_id)
    hashes = set()
    for r in releases:
        for url in (r['url'], r.get('browserUrl'), r.get('importEvidence', {}).get('filePageUrl')):
            if viking_page(url): hashes.add(url.rsplit('/', 1)[-1])
    release_ids = {r['id'] for r in releases}
    report = dict(imported=[], excluded=[], newGames=[], metadataChecksReused=0)
    for entry in entries:
        try:
            url, filename, tid = entry['file_url'], entry['filename'], entry['title_id']
            if (entry.get('host', 'Vikingfile') != 'Vikingfile' or not viking_page(url) or
                    not re.fullmatch(r'PPSA\d{5}', tid) or
                    not filename.lower().endswith(('.exfat', '.ffpfsc'))):
                raise ValueError('Not an eligible PS5 single-file Vikingfile page')
            file_hash = url.rsplit('/', 1)[-1]
            if file_hash in hashes:
                raise ValueError('File page already represented in Orbit')
            check = metadata.get(file_hash)
            if check is None:
                if (entry.get('availability') not in ('file_metadata_available', 'file_page_and_metadata_available') or
                        type(entry.get('size_bytes')) is not int or not entry.get('file_page_checked_at')):
                    raise ValueError('Exact public file metadata is missing')
                check = dict(exist=True, hash=file_hash, name=filename, size=entry['size_bytes'],
                             checked_at=entry['file_page_checked_at'])
            size = check.get('size')
            if entry.get('size_bytes') is not None and size != entry['size_bytes']:
                raise ValueError('Recorded byte sizes disagree')
            fmt = 'exFAT' if filename.lower().endswith('.exfat') else 'FFPFSC'
            release = dict(id=f'{tid.lower()}-vf-{file_hash}', titleId=tid, filename=filename,
                           url=url, browserUrl=url, sizeBytes=size, sourceId='vikingfile',
                           provider='Vikingfile', format=fmt, version=entry.get('version'), sha256='')
            if release['id'] in release_ids: raise ValueError('Duplicate release identity')
            release['verification'] = viking_metadata_evidence(release, check, check.get('checked_at'))
            release['importEvidence'] = dict(catalogueEntry=entry['entry'],
                sourceSnapshotSha256=snapshot_hash, sourceUrl=entry.get('identity_source_url'),
                sourceTitle=entry.get('source_game_title', entry['game']), sourceTags=entry.get('tags', []),
                filePageUrl=url, candidateGroup=entry.get('orbitCandidateGroup'),
                identityEvidence=entry.get('identity_evidence'),
                note='Recorded public listing evidence only. Version/region/firmware/DLC are source claims; full download and console compatibility are not verified.')
            game = by_tid.get(tid) or by_title.get(game_key(entry['game']))
            added = game is None
            if added:
                art = artwork(entry, pippo, pegasus)
                slug = re.sub(r'[^a-z0-9]+', '-', unicodedata.normalize('NFKD', entry['game'])
                              .encode('ascii', 'ignore').decode().lower()).strip('-')[:54].rstrip('-')
                if not slug or slug in by_id: slug = (slug or 'game')[:53] + '-' + tid.lower()
                game = dict(id=slug, title=entry['game'], genre=None, tagline=None, description=None,
                    publisher=None, releaseDate=None, addedAt=now(), cover=art[0][0], hero=art[0][0],
                    artworkLayout='ambient', metadata=dict(provider='Title-ID-matched public catalogue',
                        titleId=tid, url=art[0][1], sourceTitle=art[0][2], artworkChecks=[],
                        note='External artwork URLs from the saved source snapshot. No publisher/date/genre or image-probe claim is inferred.'),
                    artworkCandidates=list(dict.fromkeys(a[0] for a in art)))
                fallback = next((a[0] for a in art if a[0] != art[0][0]), None)
                if fallback: game.update(coverFallback=fallback, heroFallback=fallback)
            release['gameId'] = game['id']
            # Validate this complete option before it can alter the staged set.
            compile_catalog([game], [release])
            if added:
                games.append(game); by_id[game['id']] = game; by_title[game_key(game['title'])] = game
                report['newGames'].append(dict(id=game['id'], title=game['title']))
            by_tid[tid] = game
            releases.append(release); release_ids.add(release['id']); hashes.add(file_hash)
            report['imported'].append(dict(entry=entry['entry'], releaseId=release['id'], gameId=game['id'],
                originalGroup=entry.get('orbitCandidateGroup'), existingGame=game['id'] in original_ids))
            report['metadataChecksReused'] += 1
        except (ValueError, KeyError, TypeError) as error:
            report['excluded'].append(dict(entry=entry.get('entry'), title=entry.get('game'), reason=str(error)))
    compile_catalog(games, releases)
    report['totals'] = dict(games=len(games), releases=len(releases), addedGames=len(games)-len(original_ids),
                           addedOptions=len(report['imported']), excluded=len(report['excluded']))
    return games, releases, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('candidates', 'metadata', 'pippo', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--pegasus', type=Path, nargs='+', required=True)
    args = parser.parse_args()
    raw = args.candidates.read_bytes()
    pegasus = []
    for path in args.pegasus:
        name = 'dlps' if 'dlps' in path.name else 'pfs'
        pegasus += [dict(p, _catalogSource=f'https://pegasus-catalog.fly.dev/catalogs/{name}.json')
                    for p in json.loads(path.read_text())['packages']]
    games, releases, report = stage(json.loads(raw), json.loads((ROOT/'catalog/games.json').read_text()),
        json.loads((ROOT/'catalog/releases.json').read_text()), json.loads(args.metadata.read_text()),
        json.loads(args.pippo.read_text()), pegasus, hashlib.sha256(raw).hexdigest())
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in [('games', games), ('releases', releases), ('import-report', report)]:
        path = args.output/(name+'.json'); tmp = path.with_suffix('.tmp')
        tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2)+'\n'); tmp.replace(path)
    print(json.dumps(report['totals']))


if __name__ == '__main__': main()
