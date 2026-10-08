"""Record each download option's region from its PS5 content ID.

A PS5 title ID (PPSA04029) does not name a region; its content ID does
(U = USA, E = Europe, J = Japan, H = Asia, then P, or B for newer
publisher IDs: UP, EB, HB). Two sources give it: each
title ID's public Prosperopatches page, and the Sony store product link a game's
metadata came from, when that product is for the same title ID. A page counts
only when it is for the requested title ID, its content ID contains that title
ID, and its stated region agrees with the prefix. Where both sources exist they
must agree, or the option is left without a region.

Uses one request at a time, three seconds apart (--interval), and stops on a
challenge or rate limit like the other catalogue tools. Results are checkpointed
in the ignored .state/regions.json; checked title IDs are not requested again.
"""
import argparse
import json
import re
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import urlsplit
from catalog import Client, ROOT, region_of, STORE_PRODUCT, content_id, read, save, now, check_region


class TitlePage(HTMLParser):
    """The page's title meta tag and its 'Heading value' detail list."""
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.meta, self.fields = {}, {}
        self.item = self.heading = None
        self.small = 0

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == 'meta':
            self.meta[a.get('name') or a.get('property')] = a.get('content', '')
        elif tag == 'li' and 'bd-links-group' in (a.get('class') or '').split():
            self.item, self.heading = [], None
        elif tag == 'strong' and self.item is not None:
            self.heading = []
        elif tag == 'small':
            self.small += 1

    def handle_endtag(self, tag):
        if tag == 'small':
            self.small = max(0, self.small - 1)
        elif tag == 'strong' and self.heading is not None and self.item is not None:
            self.item.append(' '.join(''.join(self.heading).split()))
            self.item.append('')
            self.heading = None
        elif tag == 'li' and self.item and len(self.item) == 2:
            self.fields[self.item[0]] = ' '.join(self.item[1].split())
            self.item = None

    def handle_data(self, data):
        if self.item is None or self.small:
            return
        if self.heading is not None:
            self.heading.append(data)
        elif len(self.item) == 2:
            self.item[1] += data


def parse_page(text, title_id):
    """The content ID and stated region of one title ID's page."""
    page = TitlePage()
    page.feed(text)
    title = page.meta.get('twitter:title', '')
    if not title.startswith(title_id + ':') or page.fields.get('Title ID') != title_id:
        raise ValueError('Title page does not identify the requested title ID')
    content_id = page.fields.get('Content ID', '')
    if not re.fullmatch(r'[A-Z]{2}\d{4}-' + title_id + r'_00-[A-Z0-9]{16}', content_id):
        raise ValueError('Title page has no content ID for this title ID')
    return dict(contentId=content_id, pageRegion=page.fields.get('Region', ''),
                sourceTitle=title.split(':', 1)[1].strip())


def evidence(r, item, game):
    """Each source naming this option's content ID: its Prosperopatches page, and the
    Sony store product the game's metadata came from when it is for the same title ID."""
    found = []
    if item.get('status') == 'found':
        found.append(dict(provider='Prosperopatches', sourceUrl=item['sourceUrl'], contentId=item['contentId'],
                          pageRegion=item['pageRegion'], checkedAt=item['checkedAt']))
    url = game.get('metadataUrl') or ''
    match = re.fullmatch(STORE_PRODUCT + '(' + content_id(r['titleId']) + ')/?', url)
    if match:
        found.append(dict(provider='PlayStation Store', sourceUrl=url, contentId=match[1]))
    return found


def apply_regions(releases, report, games):
    """Sets each option's region where its sources agree; returns (changed, skipped)."""
    by_id = {g['id']: g for g in games}
    changed, skipped = 0, []
    for r in releases:
        sources = evidence(r, report.get(r['titleId'], {}), by_id.get(r['gameId'], {}))
        if not sources:
            continue
        prefix = sources[0]['contentId'][:2]
        candidate = dict(r, region=(region_of(prefix) or (None,))[0], regionEvidence=sources)
        try:
            check_region(candidate)
        except ValueError as exc:
            skipped.append(f"{r['id']}: {exc}")
            continue
        if (r.get('region'), r.get('regionEvidence')) != (candidate['region'], sources):
            r['region'], r['regionEvidence'] = candidate['region'], sources
            changed += 1
    return changed, skipped


def lookup(client, title_ids, report, checkpoint):
    wanted = [t for t in title_ids if report.get(t, {}).get('status') != 'found']
    for index, title_id in enumerate(wanted):
        page_url = 'https://prosperopatches.com/' + title_id
        try:
            status, _, data, final = client.request(page_url, limit=500_000)
            if status != 200 or urlsplit(final).hostname != 'prosperopatches.com':
                raise ValueError('Unexpected title page response')
            report[title_id] = dict(status='found', sourceUrl=page_url, **parse_page(data.decode('utf-8'), title_id),
                                    httpStatus=status, checkedAt=now())
        except (ValueError, OSError) as exc:  # includes HTTP errors and timeouts; retried on the next run
            report[title_id] = dict(status='unavailable', sourceUrl=page_url, error=str(exc), checkedAt=now())
        checkpoint()
        if (index + 1) % 25 == 0 or index + 1 == len(wanted):
            print(f"Checked {index + 1}/{len(wanted)}; {sum(v['status'] == 'found' for v in report.values())} found", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', help='Record agreeing regions in releases.json')
    parser.add_argument('--report', type=Path, default=ROOT / '.state/regions.json')
    parser.add_argument('--interval', type=float, default=3.0, help='Seconds between requests (at least 1)')
    args = parser.parse_args()
    releases = read('releases.json')
    report = json.loads(args.report.read_text()) if args.report.exists() else {}
    def checkpoint():
        args.report.parent.mkdir(parents=True, exist_ok=True)
        tmp = args.report.with_suffix('.tmp')
        tmp.write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')
        tmp.replace(args.report)
    lookup(Client(interval=args.interval), sorted({r['titleId'] for r in releases}), report, checkpoint)
    if args.apply:
        changed, skipped = apply_regions(releases, report, read('games.json'))
        save('releases.json', releases)
        print(f'Recorded {changed} regions. Compile the catalogue before building.', flush=True)
        for line in skipped:
            print('Skipped ' + line, flush=True)
    regions = {}
    for r in releases:
        regions[r.get('region') or 'unknown'] = regions.get(r.get('region') or 'unknown', 0) + 1
    print(json.dumps({'options': regions, 'unavailable': sorted(t for t, v in report.items() if v['status'] != 'found')}), flush=True)


if __name__ == '__main__':
    main()
