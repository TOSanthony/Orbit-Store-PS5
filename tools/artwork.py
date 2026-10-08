"""Resolve title-ID-matched external Prosperopatches artwork, with bounded image probes.

Covers: each title page names its icon. Banners (--banners): the same CDN folder holds the
title's PS5 background, pic0.webp; a 32-byte probe confirms it is a WebP and measures it,
and only a wide one becomes the game's banner.

Uses one request at a time, three seconds apart per host (--interval; the other
catalogue tools keep eight).
Only URLs and verification metadata are saved. No artwork bytes enter the repo.
"""
import argparse
import json
import re
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlsplit
import urllib.error
from catalog import Client, ROOT, read, save, now, apply_banner, check_banner

HOST = 'cdn.prosperopatches.com'


class ArtworkPage(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.meta = {}

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == 'meta':
            self.meta[a.get('name') or a.get('property')] = a.get('content', '')


def parse_page(text, title_id):
    page = ArtworkPage()
    page.feed(text)
    source_title = page.meta.get('twitter:title', '')
    url = page.meta.get('twitter:image', '')
    if not source_title.startswith(title_id + ':'):
        raise ValueError('Title page does not identify the requested title ID')
    p = urlsplit(url)
    if p.scheme != 'https' or p.hostname != HOST or p.username or p.password or p.query or p.fragment or not re.fullmatch(r'/titles/' + title_id + r'_[0-9a-f]{64}/icon0\.webp', p.path):
        raise ValueError('Page does not publish an exact-title Prosperopatches icon URL')
    return url, source_title.split(':', 1)[1].strip()


def image_signature(data):
    return (data.startswith(b'RIFF') and data[8:12] == b'WEBP') or data.startswith(b'\x89PNG\r\n\x1a\n') or data.startswith(b'\xff\xd8\xff')


def webp_size(data):
    """Width and height from a WebP's first 30 bytes, or None."""
    if len(data) < 30 or not (data.startswith(b'RIFF') and data[8:12] == b'WEBP'):
        return None
    chunk = data[12:16]
    if chunk == b'VP8X':
        return 1 + int.from_bytes(data[24:27], 'little'), 1 + int.from_bytes(data[27:30], 'little')
    if chunk == b'VP8 ' and data[23:26] == b'\x9d\x01\x2a':
        return int.from_bytes(data[26:28], 'little') & 0x3fff, int.from_bytes(data[28:30], 'little') & 0x3fff
    if chunk == b'VP8L' and data[20] == 0x2f:
        bits = int.from_bytes(data[21:25], 'little')
        return (bits & 0x3fff) + 1, ((bits >> 14) & 0x3fff) + 1
    return None


def banner_url(cover, title_ids):
    """The PS5 background (pic0) beside a title's Prosperopatches icon, or None."""
    p = urlsplit(cover)
    m = re.fullmatch(r'/titles/(PPSA\d{5})_[0-9a-f]{64}/icon0\.webp', p.path)
    if p.scheme != 'https' or p.hostname != HOST or p.username or p.password or p.query or p.fragment or not m or m.group(1) not in title_ids:
        return None
    return f'https://{HOST}{p.path.rsplit("/", 1)[0]}/pic0.webp'


def apply_banners(games, releases, report):
    """Verified wide backgrounds become banners; the publisher's banner stays as heroFallback."""
    changed = 0
    for game in games:
        item = report.get(game['id'], {})
        title_ids = {r['titleId'] for r in releases if r['gameId'] == game['id']}
        if item.get('status') != 'verified' or item.get('titleId') not in title_ids:
            continue
        banner = dict(provider='Prosperopatches', titleId=item['titleId'], url=item['url'],
                      width=item['width'], height=item['height'], checkedAt=item['checkedAt'],
                      httpStatus=item['httpStatus'], method='same title folder as the verified icon + 32-byte WebP header probe')
        check_banner(banner)
        if game.get('banner') != banner:
            game['banner'] = banner
            changed += 1
        apply_banner(game)
    return changed


def probe_banners(client, games, releases, report, save_report):
    """Checks pic0.webp beside every Prosperopatches icon not yet decided."""
    wanted = []
    for game in games:
        title_ids = {r['titleId'] for r in releases if r['gameId'] == game['id']}
        url = banner_url(game['cover'], title_ids)
        if url and report.get(game['id'], {}).get('url') != url:
            wanted.append((game, url))
    for index, (game, url) in enumerate(wanted):
        title_id = urlsplit(url).path.split('/')[2].split('_')[0]
        try:
            status, _, data, final = client.request(url, headers={'Range': 'bytes=0-31'}, limit=32)
            size = webp_size(data)
            if status != 206 or final != url or not size:
                raise ValueError('Background did not return a partial WebP response')
            wide = size[0] >= 1.4 * size[1]
            report[game['id']] = dict(status='verified' if wide else 'not-wide', titleId=title_id, url=url,
                                      width=size[0], height=size[1], httpStatus=status, checkedAt=now())
        except (ValueError, urllib.error.HTTPError) as exc:
            report[game['id']] = dict(status='unavailable', titleId=title_id, url=url, error=str(exc), checkedAt=now())
        save_report()
        if (index + 1) % 10 == 0 or index + 1 == len(wanted):
            print(f"Banners checked {index + 1}/{len(wanted)}; {sum(r['status'] == 'verified' for r in report.values())} wide", flush=True)


def apply_verified(games, releases, report):
    ids = {g['id']: {r['titleId'] for r in releases if r['gameId'] == g['id']} for g in games}
    changed = 0
    for game in games:
        item = report.get(game['id'], {})
        if item.get('status') != 'verified' or item.get('titleId') not in ids[game['id']]:
            continue
        url = item['url']
        if urlsplit(url).hostname != HOST or item['finalUrl'] != url:
            raise ValueError('Artwork verification host changed')
        for key in ('cover', 'hero'):
            previous = game.get(key, '')
            if urlsplit(previous).scheme == 'https' and previous != url and not game.get(key + 'Fallback'):
                game[key + 'Fallback'] = previous
        game['artwork'] = {
            'provider': 'Prosperopatches', 'titleId': item['titleId'], 'sourceUrl': item['sourceUrl'],
            'sourceTitle': item['sourceTitle'], 'cover': url, 'hero': url, 'artworkLayout': 'ambient',
            'checkedAt': item['checkedAt'], 'httpStatus': item['httpStatus'],
            'method': 'title page identity + 32-byte image signature probe',
        }
        game.update(cover=url, hero=url, artworkLayout='ambient')
        apply_banner(game)  # a verified banner stays the background
        changed += 1
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', help='Apply verified matches to shared artwork metadata')
    parser.add_argument('--report', type=Path, default=ROOT / '.state/artwork-prospero.json')
    parser.add_argument('--banners', action='store_true', help='Also check each Prosperopatches icon folder for a wide PS5 background')
    parser.add_argument('--banner-report', type=Path, default=ROOT / '.state/artwork-banners.json')
    parser.add_argument('--interval', type=float, default=3.0, help='Seconds between requests to one host (at least 1)')
    args = parser.parse_args()
    games, releases = read('games.json'), read('releases.json')
    wanted = [g for g in games if any(urlsplit(g[k]).hostname != HOST for k in ('cover', 'hero'))]
    report = json.loads(args.report.read_text()) if args.report.exists() else {}
    client = Client(interval=args.interval)
    def checkpoint():
        args.report.parent.mkdir(parents=True, exist_ok=True)
        tmp = args.report.with_suffix('.tmp')
        tmp.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
        tmp.replace(args.report)
    for index, game in enumerate(wanted):
        if report.get(game['id'], {}).get('status') == 'verified':
            continue
        title_id = next(r['titleId'] for r in releases if r['gameId'] == game['id'])
        page_url = 'https://prosperopatches.com/' + title_id
        try:
            status, _, data, final = client.request(page_url, limit=500_000)
            if status != 200 or urlsplit(final).hostname != 'prosperopatches.com':
                raise ValueError('Unexpected title page response')
            url, title = parse_page(data.decode('utf-8'), title_id)
            status, headers, data, final = client.request(url, headers={'Range': 'bytes=0-31'}, limit=32)
            if status != 206 or final != url or not image_signature(data):
                raise ValueError('Artwork URL did not return a valid partial image response')
            report[game['id']] = dict(status='verified', titleId=title_id, sourceUrl=page_url,
                sourceTitle=title, url=url, finalUrl=final, httpStatus=status, checkedAt=now())
        except (ValueError, urllib.error.HTTPError) as exc:
            report[game['id']] = dict(status='unavailable', titleId=title_id, sourceUrl=page_url, error=str(exc), checkedAt=now())
        checkpoint()
        if (index + 1) % 10 == 0 or index + 1 == len(wanted):
            print(f"Checked {index + 1}/{len(wanted)}; {sum(r['status'] == 'verified' for r in report.values())} verified", flush=True)
    if args.apply:
        changed = apply_verified(games, releases, report)
        save('games.json', games)
        print(f'Applied {changed} verified artwork matches. Compile the catalogue before building.', flush=True)
    missing = [g['title'] for g in wanted if report.get(g['id'], {}).get('status') != 'verified']
    print(json.dumps({'remaining': missing}, ensure_ascii=False), flush=True)
    if args.banners:
        banners = json.loads(args.banner_report.read_text()) if args.banner_report.exists() else {}
        def save_banners():
            args.banner_report.parent.mkdir(parents=True, exist_ok=True)
            tmp = args.banner_report.with_suffix('.tmp')
            tmp.write_text(json.dumps(banners, indent=2, ensure_ascii=False) + '\n')
            tmp.replace(args.banner_report)
        probe_banners(client, games, releases, banners, save_banners)
        if args.apply:
            changed = apply_banners(games, releases, banners)
            save('games.json', games)
            print(f'Applied {changed} verified banners. Compile the catalogue before building.', flush=True)
        counts = {s: sum(1 for r in banners.values() if r['status'] == s) for s in ('verified', 'not-wide', 'unavailable')}
        print(json.dumps({'banners': counts}), flush=True)


if __name__ == '__main__':
    main()
