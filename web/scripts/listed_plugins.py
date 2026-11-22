"""Refreshes co-op's listed plugins: one folder in plugins/ (plugin.json with a link and no file,
manual.md, screenshot.png) for every plugin in the live hundekraft workshop, in Opus 2's plugin
list and in op1r's release. co-op hosts no copy of these: each card links to where its author
offers it. op1r's builds also name their .op1p in op1r's repo, pinned to a commit (fileUrl), so the
page can download it checked against its hash.

A plugin on more than one of them is one folder with a build per firmware (hundekraft's port of an
op1r plugin and op1r's own): hundekraft's text and picture, op1r's build beside it. A listing
/add-plugin made from an author's repo keeps its text and builds; only its op1r build is kept here.

    python -I web/scripts/listed_plugins.py            # writes into the repository's plugins/
    python -I web/scripts/listed_plugins.py <plugins dir>

Needs Python 3 and Pillow. It reads, and never runs, these pages:

    hundekraft.github.io/op1-workshop/  catalog.json, release.json, manual.html, screenshots
    opus-op1.pages.dev/                 js/opus2-plugins.js (read as text), img/ screenshots
    github.com/pcppcp/op1r              data/manifest.json, CHANGELOG.md, README.md, docs/images/ (the HEAD commit)
    github.com/pcppcp/op1r-src          synth_sdk/plugins/*/plugin.json and README.md (the HEAD commit)

Who made each plugin is set below (AUTHORS, CREDITS), from the workshop manual's credits and op1r's
README: a port is its porter's, with the original in its credits. A new plugin in the workshop
lands under hundekraft until it is added there. Plugins with no picture get a plain name card.
Afterwards run `pnpm --dir web plugins:validate`, and check the diff for plugins that changed.
"""
import html
import io
import json
import os
import re
import sys
import time
import urllib.request

from PIL import Image, ImageDraw, ImageFont

PLUGINS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'plugins')
WORKSHOP = 'https://hundekraft.github.io/op1-workshop/'
OPUS_SITE = 'https://opus-op1.pages.dev/'
OPUS_RELEASES = 'https://github.com/LizardMan245/opus/releases'
OP1R_REPO = 'pcppcp/op1r'
OP1R_SRC_REPO = 'pcppcp/op1r-src'
OP1R_WEB = 'https://pcppcp.github.io/op1r/web/'
REFRESHED_LINKS = (WORKSHOP, OPUS_RELEASES, OP1R_WEB)
UA = {'User-Agent': 'co-op-catalogue-import'}

# ---------------------------------------------------------------- helpers

def fetch(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=30) as r:
        return r.read()


def png_ok(data):
    try:
        im = Image.open(io.BytesIO(data))
        return im.format == 'PNG' and 64 <= min(im.size) and max(im.size) <= 2048 and len(data) <= 1024 * 1024
    except Exception:
        return False


def name_card(name):
    im = Image.new('RGB', (640, 320), (20, 20, 20))
    d = ImageDraw.Draw(im)
    font = ImageFont.load_default(size=56)
    box = d.textbbox((0, 0), name, font=font)
    d.text(((640 - (box[2] - box[0])) / 2, (320 - (box[3] - box[1])) / 2 - box[1]), name, fill=(235, 235, 235), font=font)
    buf = io.BytesIO()
    im.save(buf, 'PNG', optimize=True)
    return buf.getvalue()


def md_inline(fragment):
    """An HTML fragment of the workshop manual as Markdown inline text."""
    x = re.sub(r'<a href="([^"]+)"[^>]*>(.*?)</a>', lambda m: f'[{m.group(2)}]({m.group(1)})' if m.group(1).startswith('http') else m.group(2), fragment, flags=re.S)
    x = re.sub(r'<img[^>]*alt="([^"]*)"[^>]*>', r'\1', x)
    x = re.sub(r'</?(strong|b)>', '**', x)
    x = re.sub(r'</?(em|i)>', '*', x)
    x = re.sub(r'<[^>]+>', '', x)
    return re.sub(r'\s+', ' ', html.unescape(x)).strip()


def plain(fragment):
    x = re.sub(r'<img[^>]*alt="([^"]*)"[^>]*>', r'\1', fragment)
    x = re.sub(r'<[^>]+>', '', x)
    return re.sub(r'\s+', ' ', html.unescape(x)).strip()


def clip(text, limit):
    """Text cut at a sentence end within limit characters."""
    if len(text) <= limit:
        return text
    cut = text[:limit]
    end = max(cut.rfind('. '), cut.rfind('! '), cut.rfind('? '))
    return cut[: end + 1] if end > 40 else cut[: limit - 1].rstrip() + '…'


# The fields of one firmware's build, in the order plugin.json keeps them, and the whole manifest's order.
BUILD_FIELDS = ('firmware', 'file', 'link', 'fileUrl', 'sha256', 'sizeBytes', 'version', 'requires', 'source', 'savedName')
ORDER = ('format', 'id', 'name', 'firmware', 'kind', 'author', 'version', 'description', 'license', 'file', 'link',
         'fileUrl', 'sha256', 'sizeBytes', 'screenshot', 'manual', 'source', 'requires', 'credits', 'video',
         'savedName', 'tags', 'builds')


def read_manifest(folder):
    path = os.path.join(PLUGINS, folder, 'plugin.json')
    if not os.path.exists(path):
        return None
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def builds_of(manifest):
    if 'builds' in manifest:
        return manifest['builds']
    return [{k: manifest[k] for k in BUILD_FIELDS if k in manifest}]


def ours(manifest):
    """True for a listing this script keeps: every build links to the workshop, Opus's releases or op1r's patcher.
    A listing /add-plugin took over from an author's own repo links there and knows more: its text is left alone."""
    return all(b.get('link') in REFRESHED_LINKS for b in builds_of(manifest))


def ordered(manifest):
    return {**{k: manifest[k] for k in ORDER if k in manifest}, **{k: v for k, v in manifest.items() if k not in ORDER}}


def assemble(shared, builds):
    """A plugin.json from its shared fields and its builds: the single-firmware form for one build."""
    shared = {k: v for k, v in shared.items() if k not in BUILD_FIELDS and k != 'builds'}
    return ordered({**shared, **builds[0]} if len(builds) == 1 else {**shared, 'builds': builds})


def with_build(manifest, build):
    """manifest with build in place of its build for the same firmware, or after its builds. Its other fields stay,
    in their order: it may be a listing someone else wrote."""
    old = builds_of(manifest)
    if 'builds' not in manifest and old[0]['firmware'] == build['firmware']:
        return ordered({**{k: v for k, v in manifest.items() if k not in BUILD_FIELDS}, **build})
    builds = [b for b in old if b['firmware'] != build['firmware']]
    at = next((i for i, b in enumerate(old) if b['firmware'] == build['firmware']), len(builds))
    builds.insert(at, build)
    m = {k: v for k, v in manifest.items() if k not in BUILD_FIELDS or (k == 'source' and 'builds' in manifest)}
    m['builds'] = builds
    return m


def save(path, data):
    """Writes bytes, trying again for a moment: on Windows another program (an editor, a virus scanner) can hold a
    file it just saw change, and the write then fails with EINVAL or EACCES."""
    for attempt in range(5):
        try:
            with open(path, 'wb') as f:
                f.write(data)
            return
        except OSError:
            if attempt == 4:
                raise
            time.sleep(0.5)


def save_json(folder, manifest):
    d = os.path.join(PLUGINS, folder)
    os.makedirs(d, exist_ok=True)
    save(os.path.join(d, 'plugin.json'), (json.dumps(manifest, indent=2, ensure_ascii=False) + '\n').encode('utf-8'))


def write(folder, manifest, manual, shot):
    """Writes this source's listing: its text, picture and build. A listing it shares with another source (one
    plugin on hundekraft and op1r) keeps the other's build; a listing someone else keeps is left alone."""
    old = read_manifest(folder)
    if old is not None and not ours(old):
        print('kept {} (listed from its author\'s repo)'.format(folder))
        return
    build = {k: manifest[k] for k in BUILD_FIELDS if k in manifest}
    others = [b for b in builds_of(old) if b['firmware'] != build['firmware']] if old else []
    save_json(folder, assemble(manifest, [build] + others))
    d = os.path.join(PLUGINS, folder)
    save(os.path.join(d, 'manual.md'), manual.encode('utf-8'))
    path = os.path.join(d, 'screenshot.png')
    if not same_pixels(path, shot):
        save(path, shot)


def same_pixels(path, shot):
    """True when the file on disk shows the same image: a name card drawn by another Pillow (the daily run on
    GitHub's runner) can differ in bytes but not in pixels, and is then left alone."""
    try:
        with open(path, 'rb') as f:
            old = f.read()
        if old == shot:
            return True
        a = Image.open(io.BytesIO(old)).convert('RGBA')
        b = Image.open(io.BytesIO(shot)).convert('RGBA')
        return a.size == b.size and a.tobytes() == b.tobytes()
    except Exception:
        return False


# ---------------------------------------------------------------- op1r (read first: the other listings name it)

def raw(repo, sha, path):
    return f'https://raw.githubusercontent.com/{repo}/{sha}/{path}'


def head_commit(repo):
    return json.loads(fetch(f'https://api.github.com/repos/{repo}/commits/HEAD'))['sha']


OP1R_SHA = head_commit(OP1R_REPO)
OP1R_SRC_SHA = head_commit(OP1R_SRC_REPO)
op1r_manifest = json.loads(fetch(raw(OP1R_REPO, OP1R_SHA, 'data/manifest.json')))
OP1R_RELEASE = re.search(r'^## .*\bop1r (\d+)\s*$', fetch(raw(OP1R_REPO, OP1R_SHA, 'CHANGELOG.md')).decode('utf-8'), re.M).group(1)
op1r_readme = fetch(raw(OP1R_REPO, OP1R_SHA, 'README.md')).decode('utf-8')

# op1r-src's plugin folders by the plugin name in their plugin.json (pitchdelay is "pitch delay", percdrum "perc").
src_tree = json.loads(fetch(f'https://api.github.com/repos/{OP1R_SRC_REPO}/git/trees/{OP1R_SRC_SHA}?recursive=1'))['tree']
src_files = {t['path'] for t in src_tree if t['type'] == 'blob'}
OP1R_SRC_DIR = {}
for path in sorted(src_files):
    m = re.fullmatch(r'synth_sdk/plugins/([^/]+)/plugin\.json', path)
    if m:
        try:
            OP1R_SRC_DIR.setdefault(json.loads(fetch(raw(OP1R_SRC_REPO, OP1R_SRC_SHA, path)))['name'], m.group(1))
        except Exception:
            pass

# Folders the plugin already has: hundekraft's port of op1r's grids, and ESC_CODE's own listing of Pitch Delay.
OP1R_FOLDER = {'grids': 'grids_op1r', 'pitch_delay': 'pitchdelay'}
# op1r-only listings: who made each (op1r's README credits), and their credits.
OP1R_AUTHOR = {'percsyn': 'Serhii Tonkoshkuryk', 'perc': 'Serhii Tonkoshkuryk', 'reedie': 'nvmsocool'}
OP1R_CREDITS = {
    'percsyn': 'PO-32 patch model after libpo32 by Eric Lewis (MIT); in op1r by pcppcp',
    'perc': 'PO-32 patch model after libpo32 by Eric Lewis (MIT); in op1r by pcppcp',
    'reedie': 'In op1r by pcppcp',
}
OP1R_KIND = {'synth': 'synth', 'fx': 'fx', 'drum': 'synth', 'seq': 'feature', 'sampler': 'feature', 'tool': 'feature'}
OP1R_TAGS = {'synth': ['synth'], 'fx': ['effect'], 'drum': ['drums'], 'seq': ['sequencer'], 'sampler': ['sampler'],
             'tool': ['tool']}


def op1r_folder(name):
    pid = name.replace(' ', '_')
    return OP1R_FOLDER.get(pid, pid)


def op1r_source(name):
    d = OP1R_SRC_DIR.get(name)
    return f'https://github.com/{OP1R_SRC_REPO}/tree/{OP1R_SRC_SHA}/synth_sdk/plugins/{d}' if d else f'https://github.com/{OP1R_SRC_REPO}'


def op1r_build(p):
    return {
        'firmware': 'op1/op1r',
        'link': OP1R_WEB,
        'fileUrl': raw(OP1R_REPO, OP1R_SHA, 'data/plugins/' + p['file']),
        'sha256': p['sha256'],
        'sizeBytes': p['size'],
        'version': OP1R_RELEASE,
        'requires': f'op1r {OP1R_RELEASE}',
        'source': op1r_source(p['name']),
        'savedName': p['name'],
    }


def readme_bullet(name):
    """The op1r README's bullet about a plugin: the one that starts with its bold name, else the first naming it."""
    lines = op1r_readme.splitlines()
    starts = [i for i, l in enumerate(lines) if re.match(r'\s*- \*\*%s\*\*' % re.escape(name), l, re.I)]
    names = [i for i, l in enumerate(lines) if re.match(r'\s*- ', l) and re.search(r'\*\*%s\*\*' % re.escape(name), l, re.I)]
    hits = starts or names
    if not hits:
        return ''
    i = hits[0]
    indent = len(lines[i]) - len(lines[i].lstrip())
    text = [lines[i].strip()[2:]]
    for l in lines[i + 1:]:
        if not l.strip() or (len(l) - len(l.lstrip()) <= indent and l.lstrip().startswith('- ')) or l.startswith('#'):
            break
        if len(l) - len(l.lstrip()) > indent and l.lstrip().startswith('- '):
            break  # a sub-list: the bullet's own text is enough
        text.append(l.strip())
    return re.sub(r'\s+', ' ', ' '.join(text)).strip()


def unmark(text):
    return re.sub(r'\*\*([^*]+)\*\*', r'\1', re.sub(r'\[([^\]]+)\]\([^)]+\)', r'\1', text))


def readme_intro(path):
    """The first paragraph of an op1r-src plugin README, and the README after its title."""
    text = fetch(raw(OP1R_SRC_REPO, OP1R_SRC_SHA, path)).decode('utf-8')
    body = re.sub(r'\A#[^\n]*\n+', '', text).strip()
    first = body.split('\n\n', 1)[0]
    return re.sub(r'\s+', ' ', first).strip(), body


OP1R_BY_FOLDER = {op1r_folder(p['name']): p for p in op1r_manifest['plugins']}


# ---------------------------------------------------------------- hundekraft

catalog = json.loads(fetch(WORKSHOP + 'catalog.json'))
release = json.loads(fetch(WORKSHOP + 'release.json'))
HK_VERSION = release['release']['product']['version']
manual_html = fetch(WORKSHOP + 'manual.html').decode('utf-8')

# Who made each plugin, as the workshop's manual and credits (and op1r's README) say. A port is its
# porter's, with the original credited. The rest are hundekraft's own.
OP1R = 'https://github.com/pcppcp/op1r'
HK_SRC = 'https://github.com/Hundekraft/op1-workshop'
AUTHORS = {
    'pcppcp': ['supersaw', 'fmbell', 'granular', 'tape', 'clouds', 'reverb', 'aether', 'drumplus', 'midiseq', 'midicc',
               'euclid', 'dots', 'oracle', 'peal', 'quad', 'countin', 'markers', 'punch', 'oblique', 'sysmon', 'usbout',
               'blot', 'ente', 'grids', 'dddd', 'steve', 'samplerplus'],
    'ultralazr': ['mixtur', 'arc', 'palme', 'in_a_room'],
    'Lewis-Clayton': ['nostromo', 'randomiser'],
    'nvmsocool': ['brassophone', 'windwood'],
    'AiAAAA': ['vapora'],
    'SammiBabe666': ['hollowsaint'],
    'Nola Drift': ['bloodmoon'],
    'fungiboletus': ['moai'],
    'SquareWave Studio': ['gin'],
    'Nicolas Franck Pauly': ['ploc'],
}
AUTHOR_OF = {i: a for a, ids in AUTHORS.items() for i in ids}
SOURCE = {
    'moai': 'https://github.com/fungiboletus/op-moai',
    'hollowsaint': 'https://github.com/SammiBabe666/Project_Hollow_Saint',
    'bloodmoon': 'https://github.com/SammiBabe666/Project_Bloodmoon',
    'ploc': 'https://github.com/nicolasfpauly/ploc',
    'mixtur': 'https://github.com/ultralazr/op1-musica-obscura',
    'arc': 'https://github.com/ultralazr/op1-musica-obscura',
    'palme': 'https://github.com/ultralazr/op1-musica-obscura',
    'in_a_room': 'https://github.com/ultralazr/op1-musica-obscura',
    'randomiser': 'https://github.com/pcppcp/op1r-src/tree/7cf11afc2bade31f0fb08d3af877b9bb04f970bd/synth_sdk/rnd',
    'gin': 'https://squarewave.studio',
}
LICENSE = {'moai': 'GPL-3.0-or-later', 'hollowsaint': 'GPL-3.0-or-later', 'bloodmoon': 'GPL-3.0-or-later'}
CREDITS = {
    'moai': 'fungiboletus\'s op-moai, brought into kennel by Hundekraft',
    'hollowsaint': 'SammiBabe666\'s Cathedral of the Hollow Saint, brought into kennel by Hundekraft',
    'bloodmoon': 'Nola Drift\'s Project Bloodmoon, published by SammiBabe666, brought into kennel by Hundekraft',
    'gin': 'Based on libpo32 by Eric Lewis; factory sounds adapt CNTR RNDM\'s kits. Ported to kennel by Hundekraft',
    'mixtur': 'Musica Obscura by ultralazr, from op1r, ported to kennel by Hundekraft',
    'arc': 'Musica Obscura by ultralazr, from op1r, ported to kennel by Hundekraft',
    'palme': 'Musica Obscura by ultralazr, from op1r, ported to kennel by Hundekraft',
    'in_a_room': 'Musica Obscura by ultralazr, after Alvin Lucier; from op1r, ported to kennel by Hundekraft',
    'nostromo': 'After the Deluge\'s Digital reverb by Katherine Whitlock and Jon Dattorro\'s plate; from op1r, ported to kennel by Hundekraft',
    'randomiser': 'Algorithm, history and panel by Lewis-Clayton / pcppcp; native kennel integration by Hundekraft',
    'brassophone': 'From op1r, ported to kennel by Hundekraft',
    'vapora': 'By AiAAAA and the VAPORA contributors; from op1r, ported to kennel by Hundekraft',
    'trigguard': "The idea is u/Andromax_sussybaka's",
    'blot': 'Filter after Open303\'s TB-303 filter by Robin Schmidt (MIT); from op1r, ported to kennel by Hundekraft',
    'grids': 'pcppcp\'s port of Mutable Instruments Grids by Émilie Gillet (GPL-3.0-or-later), ported to kennel by Hundekraft',
    'clouds': 'Port of Mutable Instruments Clouds by Émilie Gillet; from op1r, ported to kennel by Hundekraft',
    'reverb': 'After the Mutable Instruments Elements reverb by Émilie Gillet; screen art by DAN76; from op1r',
    'aether': 'Satellite artwork by DAN76/Rackes; from op1r, ported to kennel by Hundekraft',
    'quad': 'Lorenz waves and the Pond LFO by xoza; from op1r',
    'oblique': 'Cards by Brian Eno and Peter Schmidt; idea and card screen from ultralazr\'s op1-oblique-strategies',
    'hail': 'Port of Mutable Instruments Beads by Émilie Gillet (MIT)',
    'oxbow': 'Port of OberVave by zvenson (GPL-3.0)',
    'sid': 'Adapts Hermit (Mihaly Horvath)\'s cRSID 1.58 light renderer',
    'father': 'Concept and visual reference by xylograph; an original kennel reverb',
    'lineclock': 'Port of Pulse Sync by Volker Junginger (ESC_CODE), GPL-3.0, used with the author\'s permission',
    'strum': 'Guitar: Jeff Learman\'s Martin HD28 recordings from the Discord SFZ GM Bank (CC0)',
    'rackverb': 'Written from Eric Brombaugh\'s and thement\'s published Midiverb research; no Alesis ROM data',
    'crush': 'Reconstructs Maiko Pruett\'s #250 algorithm and artwork, used with permission',
    'warble': 'Reconstructs Maiko Pruett\'s #250 algorithm and artwork, used with permission',
    'plate': 'Reconstructs Maiko Pruett\'s #250 algorithm and artwork, used with permission',
    'chord': 'Voicings and strums from op1r',
}
for i in ('file', 'scale', 'naming', 'undo', 'tapes', 'sampletools'):
    CREDITS[i] = 'Rebuilt by hundekraft from how Maiko Pruett\'s #249 and #250 work; none of Maiko\'s code is used'
for i in ('supersaw', 'fmbell', 'granular', 'tape', 'drumplus', 'midiseq', 'midicc', 'euclid', 'dots', 'oracle', 'peal',
          'countin', 'markers', 'punch', 'sysmon', 'usbout', 'ente', 'dddd', 'steve', 'samplerplus'):
    CREDITS.setdefault(i, 'From op1r, ported to kennel by Hundekraft')

# The manual's heading id for plugins outside its data-feature sections.
ANCHOR = {'countin': 'count-in', 'fasterase': 'erase', 'sysmon': 'monitor', 'usbout': 'usb', 'tape': 'tape-fx',
          'hollowsaint': 'hollow-saint', 'juni_chorus': 'juni-chorus', 'in_a_room': 'in-a-room'}
FOLDER = {'grids': 'grids_op1r'}  # co-op already has coldsoldr's grids
# Left off co-op's hundekraft listings at their authors' request (2026-11-18): LizardMan's work and
# plugins made from it (outatime, dx, dimension, him, khorus, matrix), and displayname808's (kastle, renamed castle in hundekraft 3.3.1; Castle stays listed from their own page).
# Their Opus listings stay. A workshop entry that carries an Opus module (opmod) is LizardMan's work under a
# new id (dx_o, sim, bitcrush...), so any entry with one is left off too.
WITHDRAWN = {'outatime', 'dx', 'dimension', 'him', 'khorus', 'matrix', 'kastle', 'castle'}


def kind_of(section, entry):
    if section == 'synths':
        return 'synth'
    if section == 'effects':
        return 'fx'
    if section == 'utilities':
        return 'feature'
    return {'synth': 'synth', 'fx': 'fx', 'drum': 'synth'}.get(entry.get('kind'), 'feature')


def tags_of(section, entry, author):
    k = entry.get('kind') or {'synths': 'synth', 'effects': 'fx', 'utilities': 'feature'}[section]
    tags = {'synth': ['synth'], 'fx': ['effect'], 'seq': ['sequencer'], 'drum': ['drums'], 'feature': ['tool']}[k]
    tags = list(tags) + ['hundekraft']
    if author == 'pcppcp':
        tags.append('op1r')
    return tags


def section_html(anchor):
    m = re.search(r'<h([234])[^>]*\bid="%s"[^>]*>' % re.escape(anchor), manual_html)
    if not m:
        return None, None
    level = int(m.group(1))
    rest = manual_html[m.end():]
    stop = re.search(r'<h[2-%d][ >]|<div class="sub" data-feature=' % level, rest)
    heading = plain(manual_html[m.end(): manual_html.find('</h', m.end())])
    return heading, rest[: stop.start() if stop else 20000]


feature_anchor = {}
for fid, anchor in re.findall(r'data-feature="[a-z0-9]+:([^"]+)"[^>]*>\s*<h3[^>]*id="([^"]+)"', manual_html):
    feature_anchor.setdefault(fid, anchor)  # a feature's first section is its own; later ones are sub-topics
made = []
for section in ('synths', 'effects', 'utilities', 'op1r_plugins'):
    for e in catalog[section]:
        if not e.get('file') or e.get('reserved') or e['id'] in WITHDRAWN or e.get('opmod'):
            continue
        pid = e['id']
        anchor = feature_anchor.get(pid) or ANCHOR.get(pid) or pid
        heading, body = section_html(anchor)
        if body is None:
            sys.exit(f'no manual section for {pid} (#{anchor})')
        tagline = re.search(r'<p class="tagline">(.*?)</p>', body, re.S)
        tagline = plain(tagline.group(1)) if tagline else ''
        blocks = re.findall(r'<(p|li)>(.*?)</\1>', body, re.S)
        paras = [b for t, b in blocks if t == 'p']
        texts = [md_inline(p) for p in paras]
        # The opening text: paragraphs and list items until there is enough to say what it does.
        first, lead = '', []
        for t, b in blocks:
            if len(first) >= 80:
                break
            first = (first + ' ' + plain(b)).strip()
            lead.append(('- ' if t == 'li' else '') + md_inline(b))
        first = first or e.get('description') or e.get('type') or e['name']
        author = AUTHOR_OF.get(pid, 'hundekraft')
        desc = clip(first if len(first) > 30 or not tagline else f'{tagline} {first}', 500)
        credit_lines = [t for t in texts if re.search(r'\b(is by|by \w+ \(|brought|ported|port of|used with permission|is Maiko|artwork)', t)]
        links = []
        for href, text in re.findall(r'<a href="(https?://[^"]+)"[^>]*>(.*?)</a>', body, re.S):
            if href not in [l[0] for l in links]:
                links.append((href, plain(text)))
        shot = None
        img = re.search(r'<img src="(manual/img/[^"?]+)', body)
        for path in ([img.group(1)] if img else []) + [f'cards/{pid}.png']:
            try:
                data = fetch(WORKSHOP + path)
            except Exception:
                continue
            if png_ok(data):
                shot = data
                break
        shot = shot or name_card(e['name'])
        folder = FOLDER.get(pid, pid)
        manifest = {
            'format': 1,
            'id': folder,
            'name': e['name'],
            'firmware': 'op1/hundekraft',
            'kind': kind_of(section, e),
            'author': author,
            'description': desc,
            **({'license': LICENSE[pid]} if pid in LICENSE else {}),
            'link': WORKSHOP,
            'screenshot': 'screenshot.png',
            'manual': 'manual.md',
            'source': SOURCE.get(pid, OP1R if author == 'pcppcp' else HK_SRC),
            'requires': f'hundekraft {HK_VERSION}',
            **({'credits': clip(CREDITS[pid], 200)} if pid in CREDITS else {}),
            'tags': tags_of(section, e, author),
        }
        md = [f'# {e["name"]}', '']
        if tagline:
            md += [f'*{tagline}*', '']
        for i, t in enumerate(lead):
            md.append(t)
            # A blank line ends a paragraph; list items stay together.
            if not (t.startswith('- ') and i + 1 < len(lead) and lead[i + 1].startswith('- ')):
                md.append('')
        extra = [t for t in credit_lines if t not in lead]
        if extra or pid in CREDITS:
            md += ['## Who made it', '']
            for t in extra or [CREDITS[pid] + '.']:
                md += [t, '']
        on_op1r = OP1R_BY_FOLDER.get(folder)
        if on_op1r and 'op1r' not in manifest['tags'] and len(manifest['tags']) < 8:
            manifest['tags'].append('op1r')
        md += ['## Get it', '',
               f'{e["name"]} is part of hundekraft {HK_VERSION}, a custom firmware for the original OP-1. '
               f'Tick it in the [hundekraft workshop]({WORKSHOP}) and install it from there. '
               'co-op lists it for reference and hosts no copy.', '']
        if on_op1r:
            md += [f'On op1r {OP1R_RELEASE} it is the plugin file `{on_op1r["file"]}`: tick it in '
                   f'[op1r\'s web patcher]({OP1R_WEB}) (Connect OP-1 installs it over USB), or download it here '
                   'with op1r chosen in Settings.', '']
        md += [
               '## Links', '',
               f'- [{e["name"]} in the hundekraft manual]({WORKSHOP}manual.html#{anchor}): every control and page',
               f'- [Source]({manifest["source"]})']
        for href, text in links:
            if href != manifest['source']:
                md.append(f'- [{text or href}]({href})')
        write(folder, manifest, '\n'.join(md) + '\n', shot)
        made.append((folder, author))

# ---------------------------------------------------------------- opus

def opus_plugins(js):
    """The plugin entries of opus2-plugins.js, read as text: each entry's string fields and flags."""
    js = js[js.find('plugins: ['):]
    string = re.compile(r'\b(\w+): "((?:[^"\\]|\\.)*)"')
    out = []
    for block in re.split(r'\n\s*\{\s*(?=id: ")', js)[1:]:
        d = {}
        for k, v in string.findall(block):
            d.setdefault(k, json.loads('"' + v + '"'))
        for k in ('hidden', 'beside', 'isnew'):
            if re.search(r'\b%s: true' % k, block):
                d[k] = True
        out.append(d)
    return out


opus_js = fetch(OPUS_SITE + 'js/opus2-plugins.js').decode('utf-8')
OPUS_TAG = re.search(r'tag: "v([^"]+)"', opus_js).group(1)  # e.g. 2.0-beta.4.1
opus = [p for p in opus_plugins(opus_js) if not p.get('hidden') and p['id'] != 'opus.bridge']
opus.append({
    'id': 'opus.bridge', 'name': 'bridge', 'kind': 'sys', 'version': '0.10.0', 'img': 'opus2/bridge.png', 'beside': True,
    'text': "Runs op1r's synth engines, effects and sequencers (.op1p) and Hundekraft's (.knl) on Opus as they are, with "
            "their own screens and pages, and sounds saved with them play. An alpha, a download of its own beside the starter pack.",
})
OPUS_VERSION = OPUS_TAG.replace('-beta.', ' beta ')  # 2.0 beta 4.1
KIT = 'https://github.com/LizardMan245/opus/tree/main/plugin-kit/examples/'
OPUS_SOURCE = {x: KIT + 'opus.' + x for x in ('bitcrush', 'halloween', 'outatime', 'zoom')}
OPUS_CREDITS = {
    'dx': "Contains code derived from Google's Music Synthesizer for Android (Apache-2.0); its voices are Opus's own",
    'scales': "The idea comes from stfj's note-quantization patch for 246; written from the ground up",
    'filter': 'The OS\'s hidden filter, which wavi showed in 2016',
    'iter': 'The hidden engine found in the custom firmware thread in 2016',
    'bridge': "Loads op1r's .op1p files by popcorp and Hundekraft's .knl files; every bridged plugin stays its author's work",
}
OPUS_KIND = {'engine': 'synth', 'effect': 'fx'}
OPUS_TAGS = {'engine': ['synth'], 'effect': ['effect'], 'mod': ['lfo'], 'play': ['playing'], 'tape': ['tape'],
             'preset': ['presets'], 'look': ['theme'], 'sys': ['system']}
for p in opus:
    short = p['id'].split('.', 1)[1]
    shot = None
    if p.get('img'):
        try:
            data = fetch(OPUS_SITE + 'img/' + p['img'])
            shot = data if png_ok(data) else None
        except Exception:
            shot = None
    shot = shot or name_card(p['name'])
    text = p.get('text', '')
    manifest = {
        'format': 1,
        'id': p['id'],
        'name': p['name'],
        'firmware': 'op1/opus',
        'kind': OPUS_KIND.get(p.get('kind'), 'feature'),
        'author': 'LizardMan',
        'version': p['version'],
        'description': clip(text, 500),
        'license': 'LicenseRef-Freeware',
        'link': OPUS_RELEASES,
        'screenshot': 'screenshot.png',
        'manual': 'manual.md',
        **({'source': OPUS_SOURCE[short]} if short in OPUS_SOURCE else {}),
        'requires': f'opus {OPUS_VERSION}',
        **({'credits': clip(OPUS_CREDITS[short], 200)} if short in OPUS_CREDITS else {}),
        'savedName': p['id'],
        'tags': OPUS_TAGS.get(p.get('kind'), ['tool']) + ['opus'],
    }
    where = ('a download of its own beside the starter pack' if p.get('beside')
             else 'one file in the starter pack, or on its own')
    md = [f'# {p["name"]}', '', text, '']
    if short in OPUS_CREDITS:
        md += ['## Credits', '', OPUS_CREDITS[short] + '.', '']
    md += ['## Get it', '',
           f'{p["name"]} {p["version"]} is a plugin for Opus {OPUS_VERSION}, LizardMan\'s mod for the original OP-1 '
           f'(firmware 246): {where}, in [Opus\'s releases]({OPUS_RELEASES}). Copy the `.opmod` into the mods folder '
           'on the OP-1\'s disk. co-op lists it for reference and hosts no copy. '
           'Opus\'s plugins are © LizardMan, free to use and share.', '',
           '## Links', '',
           f'- [Opus]({OPUS_SITE}): every plugin with its keys, CPU and memory',
           f'- [Releases]({OPUS_RELEASES})',
           '- [Opus on GitHub](https://github.com/LizardMan245/opus)']
    if short in OPUS_SOURCE:
        md.append(f'- [Source in the plugin kit]({OPUS_SOURCE[short]})')
    write(p['id'], manifest, '\n'.join(md) + '\n', shot)
    made.append((p['id'], 'LizardMan'))

# ---------------------------------------------------------------- op1r
# Every op1r plugin gets an op1/op1r build: its .op1p in op1r's repo, pinned to the commit read above. A plugin with
# a listing already (hundekraft's port, or an author's own) gets the build only; an op1r-only plugin gets its own
# listing, from op1r-src's README for it or op1r's README.

def op1r_card(p, folder, build):
    name = ' '.join(w.capitalize() for w in p['name'].split(' '))
    src_dir = OP1R_SRC_DIR.get(p['name'])
    readme = f'synth_sdk/plugins/{src_dir}/README.md' if src_dir else None
    intro, body = readme_intro(readme) if readme in src_files else ('', '')
    bullet = unmark(readme_bullet(p['name']))
    # "perc (by …): a drum synth …" reads as "A drum synth …" on its own card.
    lead = re.match(r'[\w +-]{1,30}(?: \([^)]{0,60}\))?:\s+', bullet)
    if lead:
        bullet = bullet[lead.end():]
        bullet = bullet[:1].upper() + bullet[1:]
    desc = clip(intro or bullet or f'{name}, an op1r {p["kind"]} plugin.', 500)
    author = OP1R_AUTHOR.get(folder) or (p['author'] if len(p['author']) <= 60 and ';' not in p['author'] else 'pcppcp')
    shot = None
    for url in ([raw(OP1R_REPO, OP1R_SHA, f'docs/images/{k}_{folder}.png') for k in ('synth', 'fx', 'seq', 'drum', 'tool')]
                + ([raw(OP1R_SRC_REPO, OP1R_SRC_SHA, f'synth_sdk/plugins/{src_dir}/{src_dir}_preview.png')] if src_dir else [])):
        try:
            data = fetch(url)
        except Exception:
            continue
        if png_ok(data):
            shot = data
            break
    manifest = {
        'format': 1,
        'id': folder,
        'name': name,
        'kind': OP1R_KIND.get(p['kind'], 'feature'),
        'author': author,
        'description': desc,
        'license': 'GPL-3.0-or-later',
        'screenshot': 'screenshot.png',
        'manual': 'manual.md',
        **({'credits': clip(OP1R_CREDITS[folder], 200)} if folder in OP1R_CREDITS else {}),
        'tags': OP1R_TAGS.get(p['kind'], ['tool']) + ['op1r'],
        **build,
    }
    md = [f'# {name}', '', body or bullet or desc, '']
    if folder in OP1R_CREDITS:
        md += ['## Who made it', '', OP1R_CREDITS[folder] + '.', '']
    md += ['## Get it', '',
           f'{name} is a plugin of op1r {OP1R_RELEASE}, pcppcp\'s custom firmware for the original OP-1 (R246): the file '
           f'`{p["file"]}`. Tick it in [op1r\'s web patcher]({OP1R_WEB}) (Connect OP-1 installs it over USB), or '
           'download it here and send it with the patcher. co-op hosts no copy: the download comes from op1r\'s repo, '
           'checked against its hash.', '',
           '## Links', '',
           f'- [op1r](https://github.com/{OP1R_REPO}): every plugin and feature',
           f'- [Source]({build["source"]})']
    return manifest, '\n'.join(md) + '\n', shot or name_card(name)


op1r_seen = set()
for p in op1r_manifest['plugins']:
    folder = op1r_folder(p['name'])
    if folder in WITHDRAWN:
        continue
    op1r_seen.add(folder)
    build = op1r_build(p)
    old = read_manifest(folder)
    if old is None or (ours(old) and all(b['firmware'] == 'op1/op1r' for b in builds_of(old))):
        manifest, manual, shot = op1r_card(p, folder, build)
        write(folder, manifest, manual, shot)
        made.append((folder, manifest['author']))
    else:
        save_json(folder, with_build(old, build))
        print(f'op1r build on {folder}')

# An op1r plugin gone from op1r's list loses its build; a listing that was op1r's alone stays, for a person to check.
for folder in sorted(os.listdir(PLUGINS)):
    old = read_manifest(folder) if os.path.isdir(os.path.join(PLUGINS, folder)) else None
    if old is None or folder in op1r_seen:
        continue
    builds = builds_of(old)
    if not any(b['firmware'] == 'op1/op1r' and b.get('link') == OP1R_WEB for b in builds):
        continue
    rest = [b for b in builds if b['firmware'] != 'op1/op1r']
    if not rest:
        print(f'{folder} is no longer in op1r {OP1R_RELEASE}: its listing was left; remove it by hand')
        continue
    m = {k: v for k, v in old.items() if k != 'builds'}
    if len(rest) == 1 and 'source' in m and 'source' not in rest[0]:
        rest = [{**rest[0], 'source': m['source']}]
    save_json(folder, assemble(m, rest) if len(rest) == 1 else ordered({**m, 'builds': rest}))
    print(f'{folder}: op1r build dropped (no longer in op1r {OP1R_RELEASE})')

by = {}
for f, a in made:
    by.setdefault(a, []).append(f)
for a in sorted(by, key=str.lower):
    print(f'{a} ({len(by[a])}): {", ".join(by[a])}')
print(len(made), 'plugins')
