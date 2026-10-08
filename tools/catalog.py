"""Maintain the embedded catalogue. Metadata and verification only; never save artwork/game bytes.

Run with .venv/bin/python tools/catalog.py {metadata,verify,build,check}.
Network operations are explicit; normal builds use the reviewed JSON snapshot offline.
"""
import argparse
import json
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from html.parser import HTMLParser
from pathlib import Path
from catalog_crypto import FEED, GAMES, RELEASES, unseal, source_envelopes

ROOT = Path(__file__).resolve().parent.parent
CATALOG = ROOT / "catalog"
MAX_RELEASES = int(re.search(r"#define ORBIT_MAX_RELEASES (\d+)",
                            (ROOT / "backend/orbit.h").read_text())[1])
FORMAT_SUFFIXES = {'FFPFSC': '.ffpfsc', 'exFAT': '.exfat', 'FPKG': '.pkg'}


def now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


# A content ID's first letter: (the option's region, the region Prosperopatches states for it).
# The second letter is P, or B for newer publisher IDs (UB, EB, HB).
REGIONS = {'U': ('USA', 'US'), 'E': ('EUR', 'EU'), 'J': ('JPN', 'JP'), 'H': ('ASIA', 'AS')}


def region_of(prefix):
    """(region, stated region) for a content ID prefix such as EP or UB, or None."""
    return REGIONS.get(prefix[:1]) if len(prefix) == 2 and prefix[1] in 'PB' else None
STORE_PRODUCT = r'https://store\.playstation\.com/[a-z]{2}-[a-z]{2}/product/'


def content_id(title_id):
    """A content ID pattern for one title ID; group 1 is its region prefix."""
    return r'([A-Z]{2})\d{4}-' + re.escape(title_id) + r'_00-[A-Z0-9]{16}'


def check_region(r):
    """A recorded region must follow from the option's own content ID, as Sony's store
    product link or the title's Prosperopatches page gives it; every source must agree."""
    region, evidence = r.get('region'), r.get('regionEvidence')
    if region is None and evidence is None:
        return
    valid = isinstance(evidence, list) and len(evidence) > 0
    for item in evidence if valid else ():
        item = item if isinstance(item, dict) else {}
        match = re.fullmatch(content_id(r['titleId']), item.get('contentId', ''))
        expected = region_of(match[1]) if match else None
        if item.get('provider') == 'Prosperopatches':
            agrees = (expected is not None and item.get('pageRegion') == expected[1] and bool(item.get('checkedAt'))
                      and item.get('sourceUrl') == 'https://prosperopatches.com/' + r['titleId'])
        elif item.get('provider') == 'PlayStation Store':
            agrees = expected is not None and bool(
                re.fullmatch(STORE_PRODUCT + re.escape(item['contentId']) + '/?', item.get('sourceUrl', '')))
        else:
            agrees = False
        valid = valid and agrees and expected[0] == region
    if not valid:
        raise ValueError(f"Region does not follow from its content ID: {r['id']}")


def read(name):
    path = CATALOG / name
    if path.exists():
        return json.loads(path.read_text())
    purpose = {'games.json': GAMES, 'releases.json': RELEASES, 'catalog.json': FEED}[name]
    value = json.loads(unseal(path.with_suffix('.enc').read_bytes(), purpose))
    return value['releases'] if name == 'catalog.json' else value


def save(name, value):
    path = CATALOG / name
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(path)


def https(url):
    p = urllib.parse.urlsplit(url)
    return p.scheme == "https" and bool(p.hostname) and not p.username and not p.password


class Client:
    """One request at a time, eight seconds between requests to the same host.

    A challenge or rate limit ends this run, preserving the last good snapshot.
    No credentials, retries, domain rotation, or challenge-solving.
    """
    def __init__(self, interval=8.0):
        # Seconds between requests to one host. Image probes may use less; never under one.
        if interval < 1:
            raise ValueError("Keep at least one second between requests to a host")
        self.interval = interval
        self.last = {}

    def request(self, url, *, method="GET", headers=None, limit=4_000_000, data=None):
        if not https(url):
            raise ValueError("Only public HTTPS URLs are accepted")
        host = urllib.parse.urlsplit(url).hostname
        time.sleep(max(0, self.last.get(host, 0) + self.interval - time.monotonic()))
        self.last[host] = time.monotonic()
        request = urllib.request.Request(url, data=data, method=method, headers={
            "User-Agent": "OrbitStore-Catalogue/0.2 (metadata verification)",
            **(headers or {}),
        })
        try:
            with urllib.request.urlopen(request, timeout=45) as response:
                if not https(response.url):
                    raise ValueError("Non-HTTPS redirect refused")
                data = b"" if method == "HEAD" else response.read(limit + 1)
                if len(data) > limit:
                    raise ValueError("Response exceeded the metadata/probe limit")
                return response.status, dict(response.headers.items()), data, response.url
        except urllib.error.HTTPError as exc:
            if exc.code in (403, 429, 503):
                raise SystemExit(f"Stopped: HTTP {exc.code} from {host}; Retry-After: "
                                 f"{exc.headers.get('Retry-After', 'not provided')}. Try later.") from exc
            raise


class PageMetadata(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.meta = {}
        self.images = []
        self.script_type = ""
        self.script_text = ""
        self.products = {}
        self.structured = {}

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if tag == "script":
            self.script_type, self.script_text = a.get("type", ""), ""
        if tag == "meta":
            key = a.get("property") or a.get("name")
            if key and key not in self.meta:
                self.meta[key] = a.get("content", "")
        if tag in ("link", "source", "img"):
            for key in ("href", "src", "srcset", "data-src"):
                for url in re.findall(r'https://[^\s,]+', a.get(key, "")):
                    if "gmedia.playstation.com/is/image/" in url and "hero" in url.lower():
                        self.images.append(url)

    def handle_data(self, data):
        if self.script_type in ("application/json", "application/ld+json"):
            self.script_text += data

    def handle_endtag(self, tag):
        if tag != "script":
            return
        if self.script_type in ("application/json", "application/ld+json"):
            try:
                obj = json.loads(self.script_text)
                if isinstance(obj, dict):
                    if obj.get("@type") == "Product":
                        self.structured = obj
                    for key, value in obj.get("cache", {}).items():
                        if key.startswith("Product:") and isinstance(value, dict):
                            self.products.setdefault(key[8:], {}).update(value)
            except (ValueError, TypeError):
                pass
        self.script_type, self.script_text = "", ""


def parse_metadata(html, url):
    page = PageMetadata()
    page.feed(html)
    m = page.meta
    product = page.products.get(page.structured.get("sku"), {})
    if product:
        m.update({"name": product.get("name", page.structured.get("name", "")),
                  "publisher": product.get("publisherName", ""),
                  "releaseDate": product.get("releaseDate", ""),
                  "platforms": ",".join(product.get("platforms", [])),
                  "genres": ",".join(g.get("value", "") for g in product.get("localizedGenres", [])),
                  "og:image": page.structured.get("image", "")})
    title = m.get("name") or m.get("og:title", "").split(" - PS")[0]
    if not title or not https(m.get("og:image", "")):
        raise ValueError("Publisher page has no game title or HTTPS artwork")
    if m.get("platforms") and "PS5" not in m["platforms"]:
        raise ValueError("Publisher page is not for PS5")
    cover = m["og:image"]
    # Prefer the page's actual desktop hero, otherwise use cover art as ambient colour.
    heroes = [u for u in page.images if "mobile" not in u.lower() and "logo" not in u.lower()]
    heroes += [x["url"] for x in product.get("media", [])
               if x.get("type") == "IMAGE" and x.get("role") == "BACKGROUND"]
    hero = heroes[0] if heroes else cover
    if "gmedia.playstation.com/is/image/" in hero:
        hero = hero.split("?")[0] + "?$1600px$"
    elif heroes and "image.api.playstation.com/" in hero:
        hero = hero.split("?")[0] + "?w=1600"
    if "image.api.playstation.com/" in cover:
        cover = cover.split("?")[0] + "?w=600"
    return {"publisher": m.get("publisher", ""), "releaseDate": m.get("releaseDate", "")[:10],
            "cover": cover, "hero": hero, "artworkLayout": "wide" if heroes else "ambient",
            "metadata": {"provider": "PlayStation", "url": url, "sourceTitle": title,
                         "fetchedAt": now()},
            "genres": ",".join(dict.fromkeys(x.strip() for x in m.get("genres", "").split(",") if x.strip()))}


def fetch_metadata(client, games, selected):
    for g in games:
        if selected and g["id"] not in selected:
            continue
        url = g.get("metadataUrl")
        if not url:
            print(f"No reviewed publisher page yet: {g['id']}", flush=True)
            continue
        if urllib.parse.urlsplit(url).hostname not in ("www.playstation.com", "store.playstation.com"):
            raise ValueError("Metadata adapter supports official PlayStation game/store pages")
        _, _, data, _ = client.request(url)
        result = parse_metadata(data.decode(), url)
        genres = result.pop("genres")
        if genres and genres != "Unique":
            g["genre"] = genres.replace(",", " / ")
        g.update(result)
        # Reviewable exceptions for pages whose default edition has changed its release date/art.
        overrides = g.get("metadataOverrides", {})
        if overrides:
            if not overrides.get("reason") or not https(overrides.get("sourceUrl", "")):
                raise ValueError("Metadata overrides require a reason and source URL")
            for key in ("releaseDate", "cover", "hero", "artworkLayout", "genre"):
                if key in overrides:
                    g[key] = overrides[key]
        # Artwork hosting is independent of publisher metadata. Preserve reviewed
        # non-Sony URLs when a later metadata refresh parses Sony's page again.
        apply_artwork_override(g)
        apply_banner(g)
        apply_external_artwork(g)
        # Header checks only. Artwork binaries never enter the repository or cache.
        checked = []
        for art in dict.fromkeys((g["cover"], g["hero"])):
            status, headers, _, _ = client.request(art, method="HEAD")
            mime = next((v for k, v in headers.items() if k.lower() == "content-type"), "")
            if status != 200 or not mime.startswith("image/"):
                raise ValueError(f"Artwork did not return an image: {g['id']}")
            checked.append({"url": art, "contentType": mime, "checkedAt": now()})
        g["metadata"]["artworkChecks"] = checked
        save("games.json", games)
        print(f"Metadata + artwork: {g['id']}", flush=True)


def apply_artwork_override(game):
    artwork = game.get('artwork')
    if not artwork:
        return
    if artwork.get('provider') != 'Prosperopatches' or not artwork.get('checkedAt'):
        raise ValueError('Artwork override needs a verified provider')
    title_id = artwork.get('titleId', '')
    if not re.fullmatch(r'PPSA\d{5}', title_id):
        raise ValueError('Artwork override needs a PS5 title ID')
    for key in ('cover', 'hero'):
        url = artwork.get(key, '')
        parsed = urllib.parse.urlsplit(url)
        if not https(url) or parsed.hostname != 'cdn.prosperopatches.com' or not parsed.path.startswith('/titles/' + title_id + '_'):
            raise ValueError('Artwork override must use its verified title ID and CDN')
        game[key] = url
    if artwork.get('artworkLayout') != 'ambient':
        raise ValueError('Prosperopatches icons use the ambient background layout')
    game['artworkLayout'] = artwork['artworkLayout']


BANNER_HOST = 'cdn.prosperopatches.com'


def check_banner(banner):
    """A banner record: the title's PS5 background (pic0) on the cover CDN, probed and wide."""
    title_id = banner.get('titleId', '')
    url = banner.get('url', '')
    parsed = urllib.parse.urlsplit(url)
    width, height = banner.get('width'), banner.get('height')
    if banner.get('provider') != 'Prosperopatches' or not banner.get('checkedAt') or not re.fullmatch(r'PPSA\d{5}', title_id):
        raise ValueError('Banner needs a verified provider and PS5 title ID')
    if not https(url) or parsed.hostname != BANNER_HOST or parsed.query or parsed.fragment or not re.fullmatch(r'/titles/' + title_id + r'_[0-9a-f]{64}/pic0\.webp', parsed.path):
        raise ValueError('Banner must be its title ID\'s background on the cover CDN')
    # As the storefronts decide (src/Backdrop.tsx, the TV app): only a wide image is a banner.
    if not isinstance(width, int) or not isinstance(height, int) or height <= 0 or width < 1.4 * height:
        raise ValueError('Banner must be a measured wide image')


def apply_banner(game):
    """A verified banner is the game's wide background, whatever a refresh parsed."""
    banner = game.get('banner')
    if not banner:
        return
    check_banner(banner)
    game['hero'] = banner['url']
    game['artworkLayout'] = 'wide'


def apply_external_artwork(game, title_ids=None):
    """Reviewed publisher artwork for titles absent from the PS5 artwork CDN.

    Each asset carries its own provenance. Steam art is title artwork, not evidence
    of a PS5 edition/date. Neither the catalogue compiler nor refresh fetches it.
    """
    record = game.get('externalArtwork')
    if not record:
        return
    if not any(record.get(key) for key in ('cover', 'hero')):
        raise ValueError('External artwork needs at least one reviewed asset')
    if (record.get('gameId') != game.get('id') or not record.get('checkedAt') or
            not record.get('identityBasis') or
            not re.fullmatch(r'PPSA\d{5}', record.get('titleId', '')) or
            (title_ids is not None and record['titleId'] not in title_ids)):
        raise ValueError('External artwork needs a reviewed matching game identity')
    for key in ('cover', 'hero'):
        asset = record.get(key)
        if not asset:
            continue
        url, source = asset.get('url', ''), asset.get('sourceUrl', '')
        parsed, page = urllib.parse.urlsplit(url), urllib.parse.urlsplit(source)
        if (not https(url) or not https(source) or len(url.encode()) >= 2048 or
                any(ord(c) < 32 or ord(c) == 127 for c in url + source) or
                parsed.fragment or asset.get('httpStatus') != 200 or
                asset.get('contentType') not in ('image/jpeg', 'image/png', 'image/webp')):
            raise ValueError('External artwork must be a verified native-compatible HTTPS image')
        provider = asset.get('provider')
        allowed = False
        if provider == 'Steam publisher artwork':
            app = re.fullmatch(r'/app/(\d+)(?:/[^?#]*)?', page.path)
            allowed = bool(page.hostname == 'store.steampowered.com' and app and
                           parsed.hostname == 'shared.akamai.steamstatic.com' and
                           parsed.path.startswith('/store_item_assets/steam/apps/' + app[1] + '/'))
        elif provider == 'Microids':
            allowed = page.hostname == 'www.microids.com' and (
                (parsed.hostname == 'www.microids.com' and parsed.path.startswith('/wp-content/uploads/')) or
                (parsed.hostname == 'i0.wp.com' and parsed.path.startswith('/www.microids.com/wp-content/uploads/')))
        elif provider == 'Mandragora official website':
            allowed = page.hostname == parsed.hostname == 'mandragoragame.com' and parsed.path.startswith('/wp-content/uploads/')
        elif provider == 'Sega original-game artwork':
            allowed = page.hostname == parsed.hostname == 'sonic.sega.jp' and page.path.startswith('/SonicChannel/gametitle/') and parsed.path.startswith('/SonicChannel/upload_images/')
        elif provider == 'THQ Nordic':
            allowed = page.hostname == parsed.hostname == 'outcast.thqnordic.com' and parsed.path.startswith('/game-sites/outcast/')
        if not allowed:
            raise ValueError('External artwork provider and source page do not match')
        w, h = asset.get('width'), asset.get('height')
        if type(w) is not int or type(h) is not int or not 0 < w <= 8192 or not 0 < h <= 8192 or (key == 'hero' and w < 1.4 * h):
            raise ValueError('External artwork must have measured valid image dimensions')
        game[key] = url
        if key == 'hero':
            game['artworkLayout'] = 'wide'

def archive_identity(url):
    p = urllib.parse.urlsplit(url)
    parts = p.path.split("/", 3)
    if p.hostname != "archive.org" or not https(url) or len(parts) != 4 or parts[1] != "download" or p.query:
        raise ValueError("Expected a stable Archive.org download URL")
    return parts[2], urllib.parse.unquote(parts[3])


def archive_filename(source_path):
    """Keep the remote item path separate from the flat console filename."""
    parts = source_path.split('/')
    if any(not part or part == '.' or '..' in part or '\\' in part or
           any(ord(c) < 32 or ord(c) == 127 for c in part) for part in parts):
        raise ValueError('Unsafe Archive.org source path')
    return parts[-1]


def viking_identity(url):
    """Exact single-file /d/ identity, never a landing page or signed storage URL."""
    match = re.fullmatch(r'https://vikingfile\.com/d/([A-Za-z0-9]{10})/([^\s/?#\\]+)', url)
    if not match:
        raise ValueError('Expected a Vikingfile /d/ single-file URL')
    encoded = match[2]
    if re.search(r'%(?![A-Fa-f0-9]{2})', encoded) or any(ord(c) >= 127 for c in encoded):
        raise ValueError('Malformed encoded filename')
    filename = urllib.parse.unquote_to_bytes(encoded).decode('utf-8', errors='strict')
    if (not filename.lower().endswith(tuple(FORMAT_SUFFIXES.values())) or
            any(c in filename for c in ('/', '\\', '..')) or
            any(ord(c) < 32 or ord(c) == 127 for c in filename)):
        raise ValueError('Unsupported or unsafe single-file name')
    return match[1], filename


def viking_page(url):
    return isinstance(url, str) and bool(re.fullmatch(
        r'https://(?:vik1ngfile\.site|vikingfile\.com)/f/[A-Za-z0-9]{10}', url))


def viking_filename(r):
    tid, filename = r['titleId'], r['filename']
    if (not re.fullmatch(r'PPSA\d{5}', tid) or
            any(filename[m.start():m.start()+9] != tid or filename[m.start()+9:m.start()+10].isdigit()
                for m in re.finditer('PPSA', filename))):
        raise ValueError('Vikingfile filename/title ID mismatch')


def viking_metadata_evidence(r, metadata, checked_at):
    """A checked listing permits the browser flow, not a direct queued transfer."""
    if not viking_page(r['url']) or r.get('browserUrl') != r['url']:
        raise ValueError('Expected the same exact provider page for browser-only delivery')
    file_hash = r['url'].rsplit('/', 1)[-1]
    if (metadata.get('exist') is not True or metadata.get('hash', file_hash) != file_hash or
            metadata.get('name') != r['filename'] or type(metadata.get('size')) is not int or
            metadata['size'] != r['sizeBytes'] or not checked_at):
        raise ValueError('Vikingfile metadata does not match the selected file')
    return dict(kind='file-metadata', checkedAt=checked_at, url=r['url'],
                metadataUrl='https://vikingfile.com/api/check-file', fileHash=file_hash,
                exists=True, filename=r['filename'], sizeBytes=r['sizeBytes'],
                method='Public file listing; download is validated after user browser verification',
                consoleVerified=False)


def verify_viking_release(client, r):
    _, filename = viking_identity(r['url'])
    viking_filename(r)
    if filename != r['filename']:
        raise ValueError('Vikingfile filename/title ID mismatch')
    # GET signatures need not authorize HEAD. The range total is the file size.
    status, headers, data, final = client.request(
        r['url'], headers={'Range': 'bytes=0-0', 'Accept-Encoding': 'identity'}, limit=1)
    h = {k.lower(): v for k, v in headers.items()}
    if (status != 206 or h.get('content-range') != f"bytes 0-0/{r['sizeBytes']}"
            or h.get('content-length') != '1' or len(data) != 1
            or any(kind in h.get('content-type', '').lower() for kind in ('text/', 'json', 'xml'))):
        raise ValueError('Vikingfile did not return the expected one-byte file range')
    return {'checkedAt': now(), 'url': r['url'], 'method': 'GET redirect + 1-byte range',
            'httpStatus': status, 'rangeStatus': status, 'sizeBytes': r['sizeBytes'],
            'filename': filename, 'contentRange': h['content-range'], 'probeBytes': 1,
            'contentType': h.get('content-type', ''), 'etag': h.get('etag', ''),
            'downloadHost': urllib.parse.urlsplit(final).hostname, 'consoleVerified': False}


def verify_release(client, r):
    if r.get('sourceId') == 'vikingfile':
        if viking_page(r['url']):
            endpoint = 'https://vikingfile.com/api/check-file'
            status, _, data, final = client.request(endpoint, method='POST',
                data=urllib.parse.urlencode({'hash': r['url'].rsplit('/', 1)[-1]}).encode(), limit=100_000)
            if status != 200 or final != endpoint:
                raise ValueError('Unexpected Vikingfile metadata response')
            metadata = json.loads(data)
            if isinstance(metadata, list):
                if len(metadata) != 1: raise ValueError('Expected one metadata result')
                metadata = metadata[0]
            return viking_metadata_evidence(r, metadata, now())
        return verify_viking_release(client, r)
    identifier, source_path = archive_identity(r["url"])
    filename = archive_filename(source_path)
    _, _, data, _ = client.request("https://archive.org/metadata/" + identifier)
    metadata = json.loads(data)
    if metadata.get("is_dark") or metadata.get("metadata", {}).get("access-restricted-item"):
        raise ValueError("Restricted item")
    files = [f for f in metadata.get("files", []) if f.get("name") == source_path]
    if (len(files) != 1 or files[0].get("private") or filename != r["filename"]
            or not filename.lower().endswith(tuple(FORMAT_SUFFIXES.values()))):
        raise ValueError("Exact single-file listing missing")
    f = files[0]
    size = int(f["size"])
    if size != r["sizeBytes"]:
        raise ValueError(f"File size changed: expected {r['sizeBytes']}, found {size}")
    status, h, _, final = client.request(r["url"], method="HEAD")
    h = {k.lower(): v for k, v in h.items()}
    if status != 200 or int(h.get("content-length", "-1")) != size:
        raise ValueError("Download HEAD size mismatch")
    if "text/" in h.get("content-type", "") or "json" in h.get("content-type", ""):
        raise ValueError("Download returned a page")
    status, rh, data, _ = client.request(r["url"], headers={"Range": "bytes=0-15"}, limit=16)
    rh = {k.lower(): v for k, v in rh.items()}
    if status != 206 or rh.get("content-range") != f"bytes 0-15/{size}" or len(data) != 16:
        raise ValueError("Bounded range probe failed")
    return {"checkedAt": now(), "url": r["url"], "metadataUrl": "https://archive.org/metadata/" + identifier,
            "method": "metadata + HEAD + 16-byte range", "httpStatus": 200, "rangeStatus": 206,
            "sizeBytes": size, "filename": filename, "sourcePath": source_path,
            "contentType": h.get("content-type", ""),
            "etag": h.get("etag", ""), "downloadHost": urllib.parse.urlsplit(final).hostname,
            "sourceReportedHashes": {k: f[k] for k in ("md5", "sha1", "sha256") if k in f},
            "consoleVerified": False}


def compile_catalog(games, releases):
    by_id = {g["id"]: g for g in games}
    if len(by_id) != len(games) or not 1 <= len(games) <= MAX_RELEASES:
        raise ValueError("Expected unique game identities within the console capacity")
    if not 1 <= len(releases) <= MAX_RELEASES:
        raise ValueError(f"Expected 1–{MAX_RELEASES} download options")
    rows, seen, used, urls = [], set(), set(), set()
    title_ids = {}
    for r in releases:
        title_ids.setdefault(r["gameId"], set()).add(r.get("titleId"))
    for g in games:
        if g.get('externalArtwork'):
            reviewed = dict(g)
            apply_external_artwork(reviewed, title_ids.get(g['id'], ()))
            if any(reviewed.get(k) != g.get(k) for k in ('cover', 'hero', 'artworkLayout')):
                raise ValueError('Compiled artwork differs from its reviewed evidence')
        banner = g.get("banner")
        if banner:
            check_banner(banner)
            if banner["titleId"] not in title_ids.get(g["id"], ()) or g.get("hero") != banner["url"] or g.get("artworkLayout") != "wide":
                raise ValueError(f"Banner does not match its game: {g['id']}")
    fields = ("title", "genre", "tagline", "description", "cover", "hero", "coverFallback", "heroFallback", "publisher", "releaseDate", "artworkLayout", "addedAt")
    for r in releases:
        g = by_id.get(r["gameId"])
        if not g or r["id"] in seen or len(r["id"].encode()) >= 24 or len(r["gameId"].encode()) >= 64:
            raise ValueError("Invalid or duplicate game/release identity")
        if any(not g.get(k) for k in ("title", "cover", "hero", "artworkLayout")):
            raise ValueError(f"Missing shared metadata: {g['id']}")
        if len(g["title"].encode()) >= 128:
            raise ValueError("Game title exceeds the console field limit")
        if g.get("releaseDate") is not None:
            datetime.strptime(g["releaseDate"], "%Y-%m-%d")
        if g.get("addedAt") is not None:
            added = datetime.fromisoformat(g["addedAt"].replace("Z", "+00:00"))
            if added.tzinfo is None:
                raise ValueError("Catalogue addition date requires a timezone")
        if any(not https(g[k]) for k in ("cover", "hero")):
            raise ValueError("Artwork must be external HTTPS URLs")
        for key in ("coverFallback", "heroFallback"):
            value = g.get(key)
            if value is not None and (not isinstance(value, str) or not https(value) or len(value.encode()) >= 2048 or any(ord(c) < 32 or ord(c) == 127 for c in value)):
                raise ValueError("Artwork fallback must be a bounded external HTTPS URL")
        if g["artworkLayout"] not in ("wide", "ambient"):
            raise ValueError("Unsupported artwork layout")
        if r["url"] in urls or r["sourceId"] not in ("archive", "vikingfile") or r["format"] not in FORMAT_SUFFIXES:
            raise ValueError("Duplicate or unsupported download source")
        viking = r['sourceId'] == 'vikingfile'
        if 'browserUrl' in r and (not viking or not viking_page(r['browserUrl'])):
            raise ValueError('Unsupported provider browser URL')
        browser_only = viking and viking_page(r['url'])
        source_path = None if viking else archive_identity(r['url'])[1]
        if viking:
            filename = r['filename'] if browser_only else viking_identity(r['url'])[1]
        else:
            filename = archive_filename(source_path)
        if browser_only and r.get('browserUrl') != r['url']:
            raise ValueError('Browser-only options require their exact file page')
        if viking: viking_filename(r)
        if viking and r['provider'] != 'Vikingfile':
            raise ValueError('Vikingfile format/provider/title identity mismatch')
        if (filename != r["filename"] or "/" in filename or "\\" in filename or ".." in filename
                or any(ord(c) < 32 or ord(c) == 127 for c in filename)
                or len(filename.encode()) >= 160 or len(r["url"].encode()) >= 2048):
            raise ValueError("Unsafe filename or oversized URL")
        expected_suffix = FORMAT_SUFFIXES[r['format']]
        if not filename.lower().endswith(expected_suffix) or type(r["sizeBytes"]) is not int or r["sizeBytes"] <= 0:
            raise ValueError("Format/size mismatch")
        evidence = r.get("verification", {})
        if (not evidence.get('checkedAt') or evidence.get("url") != r["url"]
                or evidence.get("sizeBytes") != r["sizeBytes"] or evidence.get("filename") != filename):
            raise ValueError(f"Missing matching source verification: {r['id']}")
        if not viking and source_path != filename and evidence.get('sourcePath') != source_path:
            raise ValueError('Missing exact Archive.org subfolder verification')
        if browser_only:
            if (evidence.get('kind') != 'file-metadata' or evidence.get('exists') is not True or
                    evidence.get('metadataUrl') != 'https://vikingfile.com/api/check-file' or
                    evidence.get('fileHash') != r['url'].rsplit('/', 1)[-1]):
                raise ValueError('Missing exact public file metadata for browser-only option')
        elif evidence.get('httpStatus') != (206 if viking else 200) or evidence.get('rangeStatus') != 206:
            raise ValueError('Missing direct-download range verification')
        if viking and not browser_only and (evidence.get('probeBytes') != 1 or evidence.get('contentRange') != f"bytes 0-0/{r['sizeBytes']}"):
            raise ValueError('Missing bounded Vikingfile range evidence')
        check_region(r)
        seen.add(r["id"]); used.add(g["id"]); urls.add(r["url"])
        row = {k: v for k, v in r.items() if k not in ("verification", "importEvidence", "regionEvidence")}
        row.update({k: g.get(k) for k in fields})
        row["sourceCheckedAt"] = evidence["checkedAt"]
        row["consoleVerified"] = evidence.get("consoleVerified", False)
        if browser_only: row['sourceVerification'] = 'file-metadata'
        rows.append(row)
    if used != set(by_id):
        raise ValueError("Every game needs a verified download option")
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("metadata", "verify", "build", "check"))
    parser.add_argument("--only", nargs="*", help="game IDs (metadata) or release IDs (verify)")
    args = parser.parse_args()
    games, releases = read("games.json"), read("releases.json")
    if args.command == "metadata":
        fetch_metadata(Client(), games, args.only)
    elif args.command == "verify":
        client = Client()
        for r in releases:
            if args.only and r["id"] not in args.only:
                continue
            r["verification"] = verify_release(client, r)
            save("releases.json", releases)
            print(f"Verified single file: {r['id']}", flush=True)
    else:
        rows = compile_catalog(games, releases)
        if args.command == "build":
            save("catalog.json", rows)
            revision = json.loads((CATALOG / 'revision.json').read_text())['revision']
            for path, data in source_envelopes(CATALOG, games, releases, rows, revision):
                temporary = path.with_suffix('.tmp')
                temporary.write_bytes(data)
                temporary.replace(path)
        elif rows != read("catalog.json"):
            raise ValueError("Embedded catalogue is stale; run catalog:build")
        else:
            revision = json.loads((CATALOG / 'revision.json').read_text())['revision']
            expected = [('games.enc', games, GAMES), ('releases.enc', releases, RELEASES),
                        ('catalog.enc', {'schemaVersion': 1, 'revision': revision, 'releases': rows}, FEED)]
            for name, value, purpose in expected:
                path = CATALOG / name
                if not path.exists() or json.loads(unseal(path.read_bytes(), purpose)) != value:
                    raise ValueError('Encrypted catalogue is stale; run catalog:build')
        print(f"Catalogue: {len(games)} games, {len(rows)} validated options; artwork URLs only")


if __name__ == "__main__":
    main()
