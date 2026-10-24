#!/usr/bin/env python3
"""Download real star and galaxy catalogues and convert them for the black hole simulator.

Produces, in assets/real/ (see assets/real/README.md for the file formats):
  stars.bin      HYG v4.4 stars (Hipparcos + Yale Bright Star + Gliese), heliocentric ICRS
  galaxies.bin   McConnachie (2012) Local Group + Updated Nearby Galaxy Catalog (Karachentsev
                 et al. 2013) + 2MASS Redshift Survey (Huchra et al. 2012), heliocentric ICRS
  manifest.json  provenance (input checksums), counts and the calibration constants used

Only the Python standard library is used. Downloads go to a cache directory and are reused, so
the script works offline once the cache is filled; running it twice gives identical output.

  python3 -I tools/fetch_real_catalogs.py [--cache DIR] [--out DIR] [--offline] [--refresh]
"""

from __future__ import annotations

import argparse
import bisect
import csv
import gzip
import hashlib
import http.client
import json
import math
import os
import re
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(SCRIPT_DIR)
DEFAULT_CACHE = '/home/chubs/.claude/jobs/449dea01/tmp/real/cache'
DEFAULT_OUT = os.path.join(REPO_DIR, 'assets', 'real')

FORMAT_VERSION = 1
HEADER_BYTES = 64
STAR_RECORD = struct.Struct('<6fiIHH')       # 36 bytes
GALAXY_RECORD = struct.Struct('<12fiIHBB')   # 60 bytes
STAR_HEADER = struct.Struct('<8s8I2f4I')     # 64 bytes
GALAXY_HEADER = struct.Struct('<8s7I2f5I')   # 64 bytes
assert STAR_RECORD.size == 36 and GALAXY_RECORD.size == 60
assert STAR_HEADER.size == HEADER_BYTES and GALAXY_HEADER.size == HEADER_BYTES

# Cosmology used for redshift distances (flat Lambda-CDM).
H0 = 70.0               # km/s/Mpc
OMEGA_M = 0.3
C_KMS = 299792.458

# Hipparcos Survey completeness: V = 7.3 + 1.1 |sin b| (spectral types earlier than G5) and
# V = 7.9 + 1.1 |sin b| (later types), ESA 1997 vol. 1 sec. 1.2. HYG adds the Yale Bright Star
# and Gliese catalogues, so V = 7.5 is a safe all-sky completeness limit.
CATALOG_LIMIT_APP_MAG = 7.5

# ---------------------------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------------------------

CDS = 'https://cdsarc.cds.unistra.fr/ftp/'
CDS_HTTP = 'http://cdsarc.u-strasbg.fr/ftp/'          # same archive, plain HTTP mirror
VIZIER = ['https://vizier.cds.unistra.fr/viz-bin/asu-tsv?', 'https://vizier.cfa.harvard.edu/viz-bin/asu-tsv?']


class Source:
    def __init__(self, key, filename, urls, sha256=None, min_rows=0, columns=None, content_sha256=None,
                 csv_columns=None, title=''):
        self.key = key
        self.filename = filename
        self.urls = urls
        self.sha256 = sha256                  # pinned checksum of the whole file (static archive files)
        self.min_rows = min_rows              # VizieR query results: minimum number of data rows
        self.columns = columns                # VizieR query results: expected TSV header
        self.content_sha256 = content_sha256  # VizieR query results: pinned checksum of header + data rows
        #                                       (the files themselves are not byte-stable: comments carry dates)
        self.csv_columns = csv_columns        # CSV files without a pin: columns that must be present
        self.title = title


def cds(path):
    return [CDS + path, CDS_HTTP + path]


def vizier(query):
    return [base + query for base in VIZIER]


XSC_QUERY = ('-source=VII/233/xsc&-out=2MASX,RAJ2000,DEJ2000,K.K20e,Sb/a,Spa,r.ext'
             '&-out.max=unlimited&K.K20e=' + urllib.parse.quote('<13.0'))
RC3_QUERY = ('-source=VII/155/rc3&-out=RA2000,DE2000,name,altname,desig,PGC,type,T,D25,R25,PA,BT,B-VT,BoT,cz'
             '&-out.max=unlimited')

# HYG columns used by build_stars (both versions have them; v4.4 and v4.1 share the layout).
HYG_COLUMNS = ['id', 'hip', 'hd', 'gl', 'proper', 'ra', 'dec', 'dist', 'mag', 'spect', 'ci', 'bayer', 'flam', 'con',
               'comp']
HYG_MIN_ROWS = 110000      # v4.1 and v4.4 both list about 119,600 stars

SOURCES = {
    'hyg': Source('hyg', 'hyg_v44.csv.gz',
                  ['https://codeberg.org/astronexus/hyg/media/branch/main/data/hyg/CURRENT/hyg_v44.csv.gz'],
                  sha256='00b349893b9a53106dd488d8371e8d2fa586043e500bb3cdb8bff3931682197d',
                  title='HYG Database v4.4 (astronexus, Codeberg)'),
    # Fallback if Codeberg is unreachable: the older v4.1 on the GitHub mirror (same columns). Not
    # pinned (a mirror may re-encode it); its header and row count are checked instead.
    'hyg_fallback': Source('hyg_fallback', 'hygdata_v41.csv',
                           ['https://raw.githubusercontent.com/astronexus/HYG-Database/main/hyg/CURRENT/hygdata_v41.csv'],
                           csv_columns=HYG_COLUMNS, min_rows=HYG_MIN_ROWS,
                           title='HYG Database v4.1 (astronexus, GitHub mirror)'),
    'ungc1': Source('ungc1', 'ungc_table1.dat', cds('J/AJ/145/101/table1.dat'),
                    sha256='0d98691e4b04189e0359be7e55e8e9fa6c6d75fe085d833aad4ef5d1e101ce17',
                    title='UNGC table 1 (Karachentsev+ 2013, VizieR J/AJ/145/101)'),
    'ungc2': Source('ungc2', 'ungc_table2.dat', cds('J/AJ/145/101/table2.dat'),
                    sha256='5fb2fb2c5aac2fa7bb1e47d8861a57b7d873523f4e0c34b76657084d9bb9cde5',
                    title='UNGC table 2'),
    'ungc6': Source('ungc6', 'ungc_table6.dat', cds('J/AJ/145/101/table6.dat'),
                    sha256='199187c501d9570bd27b8d7ca5c9a923e16f7d0d1a6ba13659e4af2c7db5fa1b',
                    title='UNGC table 6 (distance moduli)'),
    'mcc1': Source('mcc1', 'mcconnachie_table1.dat', cds('J/AJ/144/4/table1.dat'),
                   sha256='26242954e8befa9159a0757cbbcbf0d40f14ffd188ef58fcf1ada0930d17fe39',
                   title='McConnachie 2012 table 1 (VizieR J/AJ/144/4)'),
    'mcc2': Source('mcc2', 'mcconnachie_table2.dat', cds('J/AJ/144/4/table2.dat'),
                   sha256='e4e7c8f76cdeff13b19168014977868c0dd25762f3e9461c72a7472f7cde3bc1',
                   title='McConnachie 2012 table 2'),
    'mcc3': Source('mcc3', 'mcconnachie_table3.dat', cds('J/AJ/144/4/table3.dat'),
                   sha256='c21658fc9632bfbf3baf84140af093249cb366274378963804d5f47221fa3ca4',
                   title='McConnachie 2012 table 3'),
    'mcc4': Source('mcc4', 'mcconnachie_table4.dat', cds('J/AJ/144/4/table4.dat'),
                   sha256='5bc9646ca622f8679fbda4b888c14fde51e242f550243cb90b347ce2fdbc6206',
                   title='McConnachie 2012 table 4'),
    'twomrs': Source('twomrs', '2mrs_table3.dat.gz', cds('J/ApJS/199/26/table3.dat.gz'),
                     sha256='14a40e14dea131afbc2ff525e42b39fdc4094cf9d06d9a43952257eff80f1790',
                     title='2MASS Redshift Survey catalogue (Huchra+ 2012, VizieR J/ApJS/199/26)'),
    # VizieR query results: the data rows are pinned by content_sha256 (264,953 and 23,011 rows when
    # pinned on 2026-10-24); min_rows only gives a clearer message for an obviously truncated reply.
    'xsc': Source('xsc', '2mass_xsc_k13.tsv', vizier(XSC_QUERY), min_rows=200000,
                  columns=['2MASX', 'RAJ2000', 'DEJ2000', 'K.K20e', 'Sb/a', 'Spa', 'r.ext'],
                  content_sha256='d864f47107b060ec5756c9858ae23e9b734f36b1c41c3a99cf2f15a760647a0d',
                  title='2MASS Extended Source Catalog shapes, K20e < 13 (VizieR VII/233)'),
    'rc3': Source('rc3', 'rc3.tsv', vizier(RC3_QUERY), min_rows=20000,
                  columns=['RA2000', 'DE2000', 'name', 'altname', 'desig', 'PGC', 'type', 'T', 'D25', 'R25',
                           'PA', 'BT', 'B-VT', 'BoT', 'cz'],
                  content_sha256='86330851f7715c8229505cb1cb73ed2ba534946f6291ef9c8ad9b8a04ea1b035',
                  title='Third Reference Catalogue of Bright Galaxies (de Vaucouleurs+ 1991, VizieR VII/155)'),
}

# ---------------------------------------------------------------------------------------------
# Download cache
# ---------------------------------------------------------------------------------------------


def log(msg):
    print(msg, flush=True)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def tsv_data_lines(path):
    """Data lines of a VizieR asu-tsv result (comments, header, units and dashes removed)."""
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        lines = [l.rstrip('\n') for l in f]
    body = [l for l in lines if l and not l.startswith('#')]
    if len(body) < 3:
        return None, []
    header = [h.strip() for h in body[0].split('\t')]
    return header, body[3:]


VERIFY_PINNED = True   # --no-verify turns the pinned-checksum checks off (structure is still checked)
PIN_HINT = ('(if the upstream data were legitimately updated, rerun with --no-verify and update the pin '
            'from manifest.json)')


def rows_sha256(header, rows):
    """Checksum of a VizieR result's header and data rows (ignores the dated comment lines)."""
    h = hashlib.sha256()
    h.update(('\t'.join(header) + '\n').encode())
    for r in rows:
        h.update((r + '\n').encode())
    return h.hexdigest()


def check_csv(src, path):
    """Structure of an unpinned CSV download: the expected columns and enough complete rows."""
    opener = gzip.open if path.endswith('.gz') else open
    try:
        with opener(path, 'rt', encoding='utf-8', newline='') as f:
            reader = csv.reader(f)
            header = next(reader, None)
            if header is None:
                raise ValueError(f'{src.filename}: empty file')
            header = [h.strip() for h in header]
            missing = [c for c in src.csv_columns if c not in header]
            if missing:
                raise ValueError(f'{src.filename}: missing columns {missing} (header {header[:12]}...)')
            n = 0
            for row in reader:
                if len(row) != len(header):
                    raise ValueError(f'{src.filename}: row {n + 2} has {len(row)} fields, expected {len(header)}; '
                                     'truncated?')
                n += 1
    except (OSError, EOFError, UnicodeDecodeError, csv.Error) as e:   # gzip/zlib errors are OSError/EOFError
        raise ValueError(f'{src.filename}: unreadable ({e})') from e
    if n < src.min_rows:
        raise ValueError(f'{src.filename}: only {n} rows (expected >= {src.min_rows}); truncated?')


def validate(src, path):
    """Raises ValueError if the cached file is not a complete, expected copy of the source."""
    if src.sha256 and VERIFY_PINNED:
        got = sha256_file(path)
        if got != src.sha256:
            raise ValueError(f'{src.filename}: sha256 {got} does not match the pinned {src.sha256} {PIN_HINT}')
    if src.columns:
        header, rows = tsv_data_lines(path)
        if header != src.columns:
            raise ValueError(f'{src.filename}: unexpected columns {header}')
        if len(rows) < src.min_rows:
            raise ValueError(f'{src.filename}: only {len(rows)} rows (expected >= {src.min_rows}); truncated?')
        if src.content_sha256 and VERIFY_PINNED:
            got = rows_sha256(header, rows)
            if got != src.content_sha256:
                raise ValueError(f'{src.filename}: content sha256 {got} ({len(rows)} rows) does not match the pinned '
                                 f'{src.content_sha256}; a truncated or changed reply {PIN_HINT}')
    if src.csv_columns:
        check_csv(src, path)


def content_sha256(src, path):
    """Checksum that ignores the date lines VizieR puts into query results."""
    if not src.columns:
        return sha256_file(path)
    header, rows = tsv_data_lines(path)
    return rows_sha256(header, rows)


def fetch(src, cache_dir, offline, refresh, required=True):
    """Returns the cached path of `src`, downloading it if needed. None if unavailable and optional."""
    path = os.path.join(cache_dir, src.filename)
    if os.path.exists(path) and not refresh:
        try:
            validate(src, path)
            return path
        except ValueError as e:
            if offline:
                raise
            log(f'  cached {src.filename} is not usable ({e}); downloading again')
    if offline:
        if required:
            raise RuntimeError(f'{src.filename} is not in the cache {cache_dir} and --offline was given')
        return None
    errors = []
    for url in src.urls:
        for _attempt in range(2):
            tmp = path + '.part'
            try:
                log(f'  downloading {url}')
                req = urllib.request.Request(url, headers={'User-Agent': 'BlackholeSimulation-catalog-fetch/1.0'})
                t0 = time.time()
                with urllib.request.urlopen(req, timeout=900) as resp, open(tmp, 'wb') as out:
                    expected = (resp.headers.get('Content-Length') or '').strip()
                    got = 0
                    while True:
                        chunk = resp.read(1 << 20)
                        if not chunk:
                            break
                        out.write(chunk)
                        got += len(chunk)
                # read(n) returns a short body without an error when a connection with a Content-Length
                # drops (only chunked replies raise IncompleteRead), so the length is checked here.
                if expected.isdigit() and got != int(expected):
                    raise ValueError(f'received {got:,} of {int(expected):,} bytes (connection dropped)')
                validate(src, tmp)
                os.replace(tmp, path)
                log(f'    {os.path.getsize(path):,} bytes in {time.time() - t0:.1f} s')
                return path
            # http.client.HTTPException covers IncompleteRead (a connection dropped mid-body), which is
            # not an OSError; URLError/HTTPError, socket and SSL errors and timeouts are OSErrors.
            except (urllib.error.URLError, OSError, ValueError, TimeoutError, http.client.HTTPException) as e:
                errors.append(f'{url}: {e}')
                log(f'    failed: {e}')
                if os.path.exists(tmp):
                    os.remove(tmp)
    if required:
        raise RuntimeError(f'could not download {src.filename}:\n  ' + '\n  '.join(errors))
    return None


# ---------------------------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------------------------


def fnum(s):
    s = s.strip()
    if not s:
        return None
    try:
        v = float(s)
    except ValueError:
        return None
    return v if math.isfinite(v) else None


def unit_vector(ra_deg, dec_deg):
    ra, dec = math.radians(ra_deg), math.radians(dec_deg)
    return (math.cos(dec) * math.cos(ra), math.cos(dec) * math.sin(ra), math.sin(dec))


def ra_dec(u):
    return math.degrees(math.atan2(u[1], u[0])) % 360.0, math.degrees(math.asin(max(-1.0, min(1.0, u[2]))))


def ang_sep_deg(u, v):
    d = max(-1.0, min(1.0, u[0] * v[0] + u[1] * v[1] + u[2] * v[2]))
    # atan2 form keeps precision for small angles
    cx = u[1] * v[2] - u[2] * v[1]
    cy = u[2] * v[0] - u[0] * v[2]
    cz = u[0] * v[1] - u[1] * v[0]
    return math.degrees(math.atan2(math.sqrt(cx * cx + cy * cy + cz * cz), d))


# IAU 1958 galactic system realised in ICRS (Hipparcos, ESA 1997 vol. 1 sec. 1.5.3): rows are the
# galactic x (towards l=0,b=0), y (l=90) and z (north galactic pole) axes in ICRS.
ICRS_TO_GAL = ((-0.0548755604162154, -0.8734370902348850, -0.4838350155487132),
               (+0.4941094278755837, -0.4448296299600112, +0.7469822444972189),
               (-0.8676661490190047, -0.1980763734312015, +0.4559837761750669))


def galactic_lb(u):
    x = sum(ICRS_TO_GAL[0][i] * u[i] for i in range(3))
    y = sum(ICRS_TO_GAL[1][i] * u[i] for i in range(3))
    z = sum(ICRS_TO_GAL[2][i] * u[i] for i in range(3))
    return math.degrees(math.atan2(y, x)) % 360.0, math.degrees(math.asin(max(-1.0, min(1.0, z))))


def mix32(x):
    """Murmur3 finaliser: deterministic 32-bit hash mixing."""
    x &= 0xFFFFFFFF
    x ^= x >> 16
    x = (x * 0x85EBCA6B) & 0xFFFFFFFF
    x ^= x >> 13
    x = (x * 0xC2B2AE35) & 0xFFFFFFFF
    x ^= x >> 16
    return x


def hash01(seed, salt):
    return mix32(seed ^ mix32(salt * 0x9E3779B9 + 0x7F4A7C15)) / 4294967296.0


def stable_seed(text):
    return zlib.crc32(text.encode('utf-8')) & 0xFFFFFFFF


class SkyIndex:
    """Grid of unit vectors for fast angular neighbour queries."""

    def __init__(self, cell_deg=0.5):
        self.s = 2.0 * math.sin(math.radians(cell_deg) / 2.0)
        self.cells = {}
        self.items = []

    def key(self, u):
        return (math.floor(u[0] / self.s), math.floor(u[1] / self.s), math.floor(u[2] / self.s))

    def add(self, u, payload):
        self.items.append((u, payload))
        self.cells.setdefault(self.key(u), []).append(len(self.items) - 1)

    def query(self, u, radius_deg):
        """Yields (separation_deg, payload) within radius_deg."""
        chord = 2.0 * math.sin(math.radians(radius_deg) / 2.0)
        if chord > self.s:
            for v, p in self.items:
                sep = ang_sep_deg(u, v)
                if sep <= radius_deg:
                    yield sep, p
            return
        kx, ky, kz = self.key(u)
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for idx in self.cells.get((kx + dx, ky + dy, kz + dz), ()):
                        v, p = self.items[idx]
                        sep = ang_sep_deg(u, v)
                        if sep <= radius_deg:
                            yield sep, p


def percentile(sorted_values, p):
    if not sorted_values:
        return None
    k = (len(sorted_values) - 1) * p
    i = int(math.floor(k))
    j = min(i + 1, len(sorted_values) - 1)
    return sorted_values[i] + (sorted_values[j] - sorted_values[i]) * (k - i)


def median(values):
    v = sorted(values)
    return percentile(v, 0.5)


# ---------------------------------------------------------------------------------------------
# Stars
# ---------------------------------------------------------------------------------------------

GREEK = {'Alp': 'Alpha', 'Bet': 'Beta', 'Gam': 'Gamma', 'Del': 'Delta', 'Eps': 'Epsilon', 'Zet': 'Zeta',
         'Eta': 'Eta', 'The': 'Theta', 'Iot': 'Iota', 'Kap': 'Kappa', 'Lam': 'Lambda', 'Mu': 'Mu', 'Nu': 'Nu',
         'Xi': 'Xi', 'Omi': 'Omicron', 'Pi': 'Pi', 'Rho': 'Rho', 'Sig': 'Sigma', 'Tau': 'Tau',
         'Ups': 'Upsilon', 'Phi': 'Phi', 'Chi': 'Chi', 'Psi': 'Psi', 'Ome': 'Omega'}

# IAU constellation abbreviations and Latin genitives (88).
GENITIVE = {
    'And': 'Andromedae', 'Ant': 'Antliae', 'Aps': 'Apodis', 'Aqr': 'Aquarii', 'Aql': 'Aquilae', 'Ara': 'Arae',
    'Ari': 'Arietis', 'Aur': 'Aurigae', 'Boo': 'Bootis', 'Cae': 'Caeli', 'Cam': 'Camelopardalis',
    'Cnc': 'Cancri', 'CVn': 'Canum Venaticorum', 'CMa': 'Canis Majoris', 'CMi': 'Canis Minoris',
    'Cap': 'Capricorni', 'Car': 'Carinae', 'Cas': 'Cassiopeiae', 'Cen': 'Centauri', 'Cep': 'Cephei',
    'Cet': 'Ceti', 'Cha': 'Chamaeleontis', 'Cir': 'Circini', 'Col': 'Columbae', 'Com': 'Comae Berenices',
    'CrA': 'Coronae Australis', 'CrB': 'Coronae Borealis', 'Crv': 'Corvi', 'Crt': 'Crateris', 'Cru': 'Crucis',
    'Cyg': 'Cygni', 'Del': 'Delphini', 'Dor': 'Doradus', 'Dra': 'Draconis', 'Equ': 'Equulei', 'Eri': 'Eridani',
    'For': 'Fornacis', 'Gem': 'Geminorum', 'Gru': 'Gruis', 'Her': 'Herculis', 'Hor': 'Horologii',
    'Hya': 'Hydrae', 'Hyi': 'Hydri', 'Ind': 'Indi', 'Lac': 'Lacertae', 'Leo': 'Leonis', 'LMi': 'Leonis Minoris',
    'Lep': 'Leporis', 'Lib': 'Librae', 'Lup': 'Lupi', 'Lyn': 'Lyncis', 'Lyr': 'Lyrae', 'Men': 'Mensae',
    'Mic': 'Microscopii', 'Mon': 'Monocerotis', 'Mus': 'Muscae', 'Nor': 'Normae', 'Oct': 'Octantis',
    'Oph': 'Ophiuchi', 'Ori': 'Orionis', 'Pav': 'Pavonis', 'Peg': 'Pegasi', 'Per': 'Persei', 'Phe': 'Phoenicis',
    'Pic': 'Pictoris', 'Psc': 'Piscium', 'PsA': 'Piscis Austrini', 'Pup': 'Puppis', 'Pyx': 'Pyxidis',
    'Ret': 'Reticuli', 'Sge': 'Sagittae', 'Sgr': 'Sagittarii', 'Sco': 'Scorpii', 'Scl': 'Sculptoris',
    'Sct': 'Scuti', 'Ser': 'Serpentis', 'Sex': 'Sextantis', 'Tau': 'Tauri', 'Tel': 'Telescopii',
    'Tri': 'Trianguli', 'TrA': 'Trianguli Australis', 'Tuc': 'Tucanae', 'UMa': 'Ursae Majoris',
    'UMi': 'Ursae Minoris', 'Vel': 'Velorum', 'Vir': 'Virginis', 'Vol': 'Volantis', 'Vul': 'Vulpeculae'}
assert len(GENITIVE) == 88

# Corrections to HYG for famous stars, keyed by HD or HIP number. Keys:
#   name         proper name (flag 32)          designation  name that is a designation (no flag 32)
#   dist_pc      literature distance (flag 16)  mag, spect   replace HYG's V magnitude / spectral type
#   teff         effective temperature, K       radius       photosphere radius, R_sun
ORION_NEBULA_PC = 388.0     # Orion Nebula Cluster, VLBA parallaxes (Kounkel et al. 2017, ApJ 834, 142)
STAR_OVERRIDES = {
    # Smith 2006 (ApJ 644, 1151) distance; HYG has no colour or usable spectral type ("pec"). The
    # luminous blue variable primary has Teff ~ 15-35 kK depending on how deep in its wind it is seen
    # (Hillier et al. 2001, ApJ 553, 837).
    ('hd', '93308'): {'name': 'Eta Carinae', 'dist_pc': 2350.0, 'teff': 15000.0},
    ('hip', '35793'): {'name': 'VY Canis Majoris', 'dist_pc': 1170.0},  # Zhang et al. 2012, ApJ 744, 23
    # The Trapezium at the heart of the Orion Nebula. Hipparcos has no usable parallaxes for these
    # blended stars, and spectroscopic distances miss their local extinction (they came out at
    # 0.98-1.18 kpc); they belong to the cluster (Gaia DR3 parallaxes 2.64 and 2.50 mas for A and C).
    # HYG gives HIP 26220 the V and spectral type of the blend; SIMBAD: HD 37020 = theta1 Ori A,
    # V 6.73, B0V. Components are named as in the literature (theta1 Ori B is not in Hipparcos).
    ('hip', '26220'): {'designation': 'Theta1 Orionis A', 'dist_pc': ORION_NEBULA_PC, 'mag': 6.73, 'spect': 'B0V'},
    ('hip', '26221'): {'designation': 'Theta1 Orionis C', 'dist_pc': ORION_NEBULA_PC},    # HD 37022, O7Vp
    ('hip', '26224'): {'designation': 'Theta1 Orionis D', 'dist_pc': ORION_NEBULA_PC},    # HD 37023, B0.5Vp
    # The other bright cluster members: Hipparcos parallaxes of 2.1 and 2.3 mas (+-0.8) put them 86 and
    # 56 pc behind the nebula (Gaia DR3 for theta2 Ori A: 2.97 +- 0.21 mas).
    ('hip', '26235'): {'dist_pc': ORION_NEBULA_PC},                                       # theta2 Ori A
    ('hip', '26258'): {'designation': 'NU Orionis', 'dist_pc': ORION_NEBULA_PC},          # HD 37061, ionises M43
    # Mira (omicron Ceti): HYG's B-V (0.97) of this large-amplitude M5e-M9e variable gives 4800 K
    # and 7 R_sun. Interferometry: Teff ~ 3000 K, R ~ 330-400 R_sun (Woodruff et al. 2004, A&A 421,
    # 703; Perrin et al. 2004, A&A 426, 279).
    ('hip', '10826'): {'teff': 3000.0, 'radius': 350.0},
}
# Spectroscopic distances of stars without usable parallaxes are limited to this range (Gliese stars:
# NEAR_STAR_RANGE_PC). The limits are soft: each star gets its own limit, spread log-uniformly over a
# factor 10^0.25 inside the range by a hash of its HYG id, so no shell of stars forms at the limit
# (hard limits had put 1,189 stars at exactly 400 pc and 100 at 6000 pc).
EST_RANGE_PC = (400.0, 6000.0)
NEAR_STAR_RANGE_PC = (5.0, 25.0)
LIMIT_SPREAD_DEX = 0.25


def soft_limits(dist, lo, hi, key):
    """Clamps dist into [lo, hi] with per-star limits lo*10^(s*u1) and hi*10^(-s*u2) (u from a hash)."""
    lo_k = lo * 10.0 ** (LIMIT_SPREAD_DEX * hash01(key, 31))
    hi_k = hi * 10.0 ** (-LIMIT_SPREAD_DEX * hash01(key, 32))
    return min(max(dist, lo_k), hi_k)
SKIP_PROPER = {'3C 273'}   # a quasar listed in HYG with a proper name; not a star
# Largest well-measured stellar radii are ~1500 R_sun (e.g. VY CMa, Wittkowski et al. 2012); radii
# estimated from reddened colours and spectroscopic distances can exceed that and are capped.
MAX_RADIUS_SOLAR = 1800.0

# Spectral sequence index: O0 = 0, B0 = 10, A0 = 20, F0 = 30, G0 = 40, K0 = 50, M0 = 60.
# Teff: dwarfs after Pecaut & Mamajek (2013), giants after Alonso et al. (1999) / Gray (2005).
# M_V per luminosity class after Schmidt-Kaler (1982, Landolt-Boernstein VI/2b), smoothed.
#            idx   Teff V  Teff III   V      IV     III    II     Ib     Iab    Ia
SPECTRAL = [(5,  41400, 40000, -5.7, -5.8, -6.0, -6.3, -6.5, -6.7, -7.0),
            (7,  37100, 36000, -5.2, -5.4, -5.7, -6.0, -6.3, -6.6, -7.0),
            (9,  33300, 32000, -4.5, -4.8, -5.3, -5.8, -6.2, -6.5, -7.0),
            (10, 31400, 30000, -4.0, -4.4, -5.1, -5.6, -6.1, -6.5, -7.0),
            (12, 20600, 20000, -2.4, -3.0, -3.9, -5.0, -5.7, -6.4, -7.1),
            (15, 15700, 15000, -1.2, -1.8, -2.2, -4.3, -5.4, -6.2, -7.1),
            (18, 12300, 12000, -0.25, -0.8, -1.2, -3.5, -5.0, -6.2, -7.1),
            (20, 9700, 9700, 0.65, 0.2, 0.0, -2.8, -4.9, -6.3, -7.1),
            (25, 8100, 8100, 1.95, 1.4, 0.7, -2.4, -4.8, -6.6, -7.4),
            (30, 7200, 7150, 2.7, 2.0, 1.5, -2.2, -4.7, -6.6, -7.5),
            (35, 6510, 6450, 3.5, 2.4, 1.6, -2.1, -4.6, -6.6, -7.6),
            (40, 5920, 5600, 4.4, 3.0, 1.0, -2.1, -4.5, -6.4, -7.6),
            (42, 5770, 5400, 4.7, 3.1, 0.9, -2.1, -4.5, -6.3, -7.6),
            (45, 5660, 5050, 5.1, 3.2, 0.9, -2.2, -4.5, -6.2, -7.5),
            (48, 5490, 4800, 5.5, 3.2, 0.8, -2.2, -4.5, -6.1, -7.5),
            (50, 5280, 4660, 5.9, 3.2, 0.7, -2.3, -4.4, -6.0, -7.5),
            (52, 4990, 4400, 6.4, 3.3, 0.5, -2.3, -4.4, -5.9, -7.5),
            (55, 4450, 3980, 7.35, 3.5, -0.2, -2.4, -4.5, -5.8, -7.5),
            (57, 4050, 3900, 8.1, 3.7, -0.3, -2.4, -4.5, -5.7, -7.5),
            (60, 3850, 3800, 8.8, 4.0, -0.4, -2.4, -4.5, -5.6, -7.5),
            (62, 3560, 3650, 9.9, 4.5, -0.6, -2.4, -4.5, -5.6, -7.5),
            (65, 3060, 3400, 12.3, 5.0, -0.3, -2.4, -4.5, -5.6, -7.5),
            (68, 2570, 2900, 16.0, 6.0, 0.0, -2.0, -4.0, -5.0, -7.0)]
LUM_COLUMN = {'V': 3, 'IV': 4, 'III': 5, 'II': 6, 'Ib': 7, 'Iab': 8, 'Ia': 9}
LUM_RE = re.compile(r'(Ia0|Ia\+|Iab|Ia|Ib|III|II|IV|VI|V|I)')


def spectral_row(idx):
    if idx <= SPECTRAL[0][0]:
        return SPECTRAL[0]
    if idx >= SPECTRAL[-1][0]:
        return SPECTRAL[-1]
    keys = [r[0] for r in SPECTRAL]
    j = bisect.bisect_right(keys, idx)
    a, b = SPECTRAL[j - 1], SPECTRAL[j]
    t = (idx - a[0]) / (b[0] - a[0])
    return tuple(a[k] + (b[k] - a[k]) * t for k in range(len(a)))


def parse_spectral(spect):
    """Returns dict(kind, idx, lum) or None. kind: 'normal', 'wd', 'wr', 'carbon', 'subdwarf'."""
    s = spect.strip()
    if not s:
        return None
    prefix = ''
    m = re.match(r'^(sd|d|g|c)(?=[OBAFGKM])', s)
    if m:
        prefix = m.group(1)
        s = s[len(prefix):]
    if s.startswith('k'):                    # Am-star notation kA2hA5mA7V: use the hydrogen type
        m = re.search(r'h([OBAFGKM]\d(?:\.\d)?)', s)
        s = (m.group(1) + s[m.end():]) if m else s[1:]
    c = s[:1]
    if not c:
        return None
    if c in 'OBAFGKM':
        m = re.match(r'([OBAFGKM])\s*(\d(?:\.\d+)?)?', s)
        sub = float(m.group(2)) if m.group(2) else 5.0
        idx = 'OBAFGKM'.index(c) * 10 + min(sub, 9.5)
        lm = LUM_RE.search(s[m.end():])
        lum = lm.group(1) if lm else None
        if lum in ('Ia0', 'Ia+'):
            lum = 'Ia0'
        elif lum == 'I':
            lum = 'Iab'
        elif lum == 'VI':
            prefix = 'sd'
            lum = 'V'
        if lum is None:
            lum = {'d': 'V', 'sd': 'V', 'g': 'III', 'c': 'Iab'}.get(prefix)
        return {'kind': 'subdwarf' if prefix == 'sd' else 'normal', 'idx': idx, 'lum': lum}
    if c == 'D':
        m = re.match(r'D[ABCOQZX]*\s*(\d+(?:\.\d+)?)', s)
        n = float(m.group(1)) if m else 5.0
        return {'kind': 'wd', 'teff': 50400.0 / max(n, 1.0)}
    if c == 'W':
        return {'kind': 'wr'}
    if c in 'CRNS':
        return {'kind': 'carbon', 'teff': 4000.0 if c == 'R' else (3200.0 if c == 'S' else 3000.0)}
    return None


def spectral_teff(sp):
    if sp is None:
        return None
    if sp['kind'] in ('wd', 'carbon'):
        return sp['teff']
    if sp['kind'] == 'wr':
        return 45000.0
    row = spectral_row(sp['idx'])
    giant = sp['lum'] in ('III', 'II', 'Ib', 'Iab', 'Ia', 'Ia0')
    return row[2] if (giant and sp['idx'] >= 40) else row[1]


def spectral_absmag(sp, lum=None):
    if sp is None:
        return None
    if sp['kind'] == 'wd':
        return 11.5
    if sp['kind'] == 'wr':
        return -4.5
    if sp['kind'] == 'carbon':
        return -1.0
    lum = lum or sp['lum']
    if lum == 'Ia0':
        return -8.5
    row = spectral_row(sp['idx'])
    mv = row[LUM_COLUMN.get(lum, 3)]
    if sp['kind'] == 'subdwarf':
        mv += 1.5
    return mv


def ballesteros_teff(bv):
    """Ballesteros (2012, EPL 97, 34008): blackbody Teff from B-V (21700 K .. 2650 K after clamping)."""
    bv = min(max(bv, -0.40), 2.60)
    return 4600.0 * (1.0 / (0.92 * bv + 1.7) + 1.0 / (0.92 * bv + 0.62))


def bolometric_correction(teff):
    """BC_V(Teff), Flower (1996) polynomials with the corrected coefficients of Torres (2010, AJ 140, 1158)."""
    lt = math.log10(min(max(teff, 3200.0), 50000.0))
    if lt < 3.70:
        c = (-0.190537291496456e+05, 0.155144866764412e+05, -0.421278819301717e+04, 0.381476328422343e+03)
    elif lt < 3.90:
        c = (-0.370510203809015e+05, 0.385672629965804e+05, -0.150651486316025e+05, 0.261724637119416e+04,
             -0.170623810323864e+03)
    else:
        c = (-0.118115450538963e+06, 0.137145973583929e+06, -0.636233812100225e+05, 0.147412923562646e+05,
             -0.170587278406872e+04, 0.788731721804990e+02)
    bc = 0.0
    for k, ck in enumerate(c):
        bc += ck * lt ** k
    return max(bc, -4.5)


def radius_solar(absmag_v, teff):
    """Stefan-Boltzmann radius from V-band absolute magnitude and Teff (M_bol,sun = 4.74, IAU 2015 B2)."""
    mbol = absmag_v + bolometric_correction(teff)
    lum = 10.0 ** (-0.4 * (mbol - 4.74))
    return math.sqrt(lum) * (5772.0 / teff) ** 2


def extinction_v(d_pc, b_deg):
    """Cosecant-law estimate of A_V: 0.7 mag/kpc in the midplane, dust scale height 110 pc."""
    a0, h = 0.7e-3, 110.0
    sb = abs(math.sin(math.radians(b_deg)))
    if sb < 1e-3:
        return a0 * d_pc
    return a0 * h / sb * (1.0 - math.exp(-d_pc * sb / h))


def photometric_distance(m, mv, b_deg):
    """Distance (pc) where m = M + 5 log10(d/10) + A_V(d, b), by bisection."""
    lo, hi = 1.0, 100000.0
    for _ in range(80):
        mid = math.sqrt(lo * hi)
        if mv + 5.0 * math.log10(mid / 10.0) + extinction_v(mid, b_deg) < m:
            lo = mid
        else:
            hi = mid
    return math.sqrt(lo * hi)


def read_hyg(path):
    opener = gzip.open if path.endswith('.gz') else open
    with opener(path, 'rt', encoding='utf-8', newline='') as f:
        return list(csv.DictReader(f))


def designation(row):
    """Bayer or Flamsteed designation in full Latin form, e.g. 'Tau Ceti', '61 Cygni', 'Pi5 Orionis'."""
    con = row['con'].strip()
    gen = GENITIVE.get(con)
    if not gen:
        return None
    bayer = row['bayer'].strip()
    if bayer:
        base, _, num = bayer.partition('-')
        letter = GREEK.get(base, base)
        return f'{letter}{num} {gen}'
    flam = row['flam'].strip()
    if flam:
        return f'{flam} {gen}'
    return None


def gliese_name(gl):
    gl = gl.strip()
    m = re.match(r'^(Gl|GJ|NN|Wo)\s*(\d+(?:\.\d+)?)\s*([A-Z]*)$', gl)
    if not m:
        return None
    cat = {'Gl': 'Gliese', 'GJ': 'GJ', 'NN': 'GJ', 'Wo': 'GJ'}[m.group(1)]
    return f'{cat} {m.group(2)}' + (f' {m.group(3)}' if m.group(3) else '')


def build_stars(rows, stats):
    stars = []          # dicts
    n_est = n_spec_teff = n_hot = n_limited = n_override = 0
    matched_overrides = set()
    for r in rows:
        proper = r['proper'].strip()
        if proper in SKIP_PROPER:
            stats['skipped_nonstellar'] = stats.get('skipped_nonstellar', 0) + 1
            continue
        hip = r['hip'].strip()
        hd = r['hd'].strip()
        ov_key = next((k for k in (('hip', hip), ('hd', hd)) if k[1] and k in STAR_OVERRIDES), None)
        ov = STAR_OVERRIDES[ov_key] if ov_key else {}
        if ov_key:
            matched_overrides.add(ov_key)
            n_override += 1
        mag = ov['mag'] if 'mag' in ov else fnum(r['mag'])
        dist = fnum(r['dist'])
        ra_h, dec = fnum(r['ra']), fnum(r['dec'])
        if mag is None or dist is None or ra_h is None or dec is None:
            stats['skipped_incomplete'] = stats.get('skipped_incomplete', 0) + 1
            continue
        flags = 0
        is_sun = r['id'].strip() == '0'
        sp = parse_spectral(ov['spect'] if 'spect' in ov else r['spect'])
        ci = fnum(r['ci'])
        # Effective temperature: Ballesteros (2012) from B-V; spectral type where B-V is missing,
        # and for O/B/WR stars where B-V saturates (and is reddened) the hotter spectral value.
        if is_sun:
            teff = 5772.0
        elif ci is not None:
            teff = ballesteros_teff(ci)
            if sp is not None and (sp['kind'] == 'wr' or (sp['kind'] in ('normal', 'subdwarf') and sp['idx'] < 20)):
                ts = spectral_teff(sp)
                if ts > teff:
                    teff = ts
                    n_hot += 1
                    flags |= 4
        else:
            ts = spectral_teff(sp)
            teff = ts if ts else 5800.0
            flags |= 2
            n_spec_teff += 1
        if 'teff' in ov:
            teff = ov['teff']
        u = unit_vector(ra_h * 15.0, dec)
        if 'dist_pc' in ov:
            dist = ov['dist_pc']
            flags |= 16
        elif dist >= 100000.0 and not is_sun:
            # No usable parallax (HYG marks parallax < 1 mas or missing with 100000 pc): estimate the
            # distance from the spectral type and luminosity class, with a cosecant extinction law.
            _, b = galactic_lb(u)
            near_star = bool(r['gl'].strip())    # Gliese stars are within ~25 pc
            mv = None
            if sp is not None:
                if sp['kind'] in ('normal', 'subdwarf') and sp['lum'] is None and not near_star:
                    # Unknown class: the least luminous class that is consistent with the
                    # insignificant parallax (distance > ~400 pc).
                    order = ['V', 'IV', 'III', 'II', 'Ib'] if sp['idx'] < 40 else ['III', 'II', 'Ib']
                    for lc in order:
                        mv = spectral_absmag(sp, lc)
                        if photometric_distance(mag, mv, b) >= 400.0:
                            break
                else:
                    mv = spectral_absmag(sp, 'V' if (near_star and sp.get('lum') is None) else None)
            if mv is None:
                mv = 9.0 if near_star else 0.5
            dist = photometric_distance(mag, mv, b)
            lo, hi = NEAR_STAR_RANGE_PC if near_star else EST_RANGE_PC
            if not lo <= dist <= hi:
                n_limited += 1
            dist = soft_limits(dist, lo, hi, int(r['id']))
            flags |= 1
            n_est += 1
        if is_sun:
            pos = (0.0, 0.0, 0.0)
            absmag = 4.83
            radius = 1.0
        else:
            pos = (u[0] * dist, u[1] * dist, u[2] * dist)
            absmag = mag - 5.0 * math.log10(dist / 10.0)
            radius = ov['radius'] if 'radius' in ov else min(radius_solar(absmag, teff), MAX_RADIUS_SOLAR)
        comp = r['comp'].strip()
        if comp and comp != '1':
            flags |= 8
        # Names: proper name, then Bayer/Flamsteed, then Gliese for stars within 10 pc.
        name, proper_flag = None, False
        if is_sun:
            name, proper_flag = 'Sun', True
        elif 'name' in ov:
            name, proper_flag = ov['name'], True
        elif 'designation' in ov:
            name = ov['designation']
        elif proper:
            name, proper_flag = proper, True
        else:
            name = designation(r)
            if name is None and dist < 10.0 and r['gl'].strip():
                name = gliese_name(r['gl'])
        if proper_flag:
            flags |= 32
        stars.append({'pos': pos, 'absmag': absmag, 'teff': teff, 'radius': radius, 'name': name,
                      'hip': int(hip) if hip.isdigit() else 0, 'flags': flags, 'mag': mag, 'dist': dist,
                      'comp': int(comp) if comp.isdigit() else 1, 'sun': is_sun, 'hyg_id': int(r['id'])})
    # Unique names: components sharing a designation get " A", " B", ... in component order.
    groups = {}
    for i, s in enumerate(stars):
        if s['name']:
            groups.setdefault(s['name'], []).append(i)
    for name, idxs in groups.items():
        if len(idxs) > 1:
            idxs.sort(key=lambda i: (stars[i]['comp'], stars[i]['mag'], stars[i]['hyg_id']))
            for k, i in enumerate(idxs):
                stars[i]['name'] = f'{name} {chr(ord("A") + k)}' if k < 26 else f'{name} {k + 1}'
    # The Sun first.
    stars.sort(key=lambda s: (not s['sun'], s['hyg_id']))
    assert stars[0]['sun']
    unmatched = sorted(set(STAR_OVERRIDES) - matched_overrides)
    if unmatched:
        raise ValueError(f'STAR_OVERRIDES without a HYG row: {unmatched}')
    stats['stars_distance_estimated'] = n_est
    stats['stars_distance_soft_limited'] = n_limited
    stats['stars_teff_from_spectral_type'] = n_spec_teff
    stats['stars_teff_hot_correction'] = n_hot
    stats['stars_overridden'] = n_override
    return stars


# ---------------------------------------------------------------------------------------------
# Galaxies
# ---------------------------------------------------------------------------------------------

MORPH_SPIRAL, MORPH_BARRED, MORPH_LENTICULAR, MORPH_ELLIPTICAL, MORPH_IRREGULAR = 0, 1, 2, 3, 4

G_BARRED, G_WEAKBAR, G_PA, G_CZ, G_MCC, G_UNGC, G_2MRS, G_RC3, G_DSPH, G_TYPE_GUESS, G_CLUSTER = (
    1 << 0, 1 << 1, 1 << 2, 1 << 3, 1 << 4, 1 << 5, 1 << 6, 1 << 7, 1 << 8, 1 << 9, 1 << 10)
DIST_HUBBLE, DIST_DIRECT, DIST_CLUSTER, DIST_PHOTOMETRIC, DIST_LITERATURE = 0, 1, 2, 3, 4

# Messier galaxies (M102 is ambiguous and omitted).
MESSIER_NGC = {31: 224, 32: 221, 33: 598, 49: 4472, 51: 5194, 58: 4579, 59: 4621, 60: 4649, 61: 4303,
               63: 5055, 64: 4826, 65: 3623, 66: 3627, 74: 628, 77: 1068, 81: 3031, 82: 3034, 83: 5236,
               84: 4374, 85: 4382, 86: 4406, 87: 4486, 88: 4501, 89: 4552, 90: 4569, 91: 4548, 94: 4736,
               95: 3351, 96: 3368, 98: 4192, 99: 4254, 100: 4321, 101: 5457, 104: 4594, 105: 3379,
               106: 4258, 108: 3556, 109: 3992, 110: 205}
NGC_MESSIER = {v: k for k, v in MESSIER_NGC.items()}

# Common names keyed by canonical identifier.
COMMON_NAMES = {
    'M31': 'Andromeda Galaxy', 'M33': 'Triangulum Galaxy', 'M51': 'Whirlpool Galaxy', 'M63': 'Sunflower Galaxy',
    'M64': 'Black Eye Galaxy', 'M74': 'Phantom Galaxy', 'M77': 'Cetus A', 'M81': "Bode's Galaxy",
    'M82': 'Cigar Galaxy', 'M83': 'Southern Pinwheel Galaxy', 'M87': 'Virgo A', 'M101': 'Pinwheel Galaxy',
    'M104': 'Sombrero Galaxy', 'NGC5128': 'Centaurus A', 'NGC253': 'Sculptor Galaxy', 'NGC4565': 'Needle Galaxy',
    'NGC4631': 'Whale Galaxy', 'NGC4038': 'Antennae Galaxies', 'NGC4039': 'Antennae Galaxies',
    'NGC1316': 'Fornax A', 'NGC1275': 'Perseus A', 'NGC6822': "Barnard's Galaxy",
    'NGC1365': 'Great Barred Spiral Galaxy', 'IC342': 'Hidden Galaxy', 'ESO97-13': 'Circinus Galaxy',
    'LMC': 'Large Magellanic Cloud', 'SMC': 'Small Magellanic Cloud', 'WLM': 'Wolf-Lundmark-Melotte Galaxy',
}

# Local Group names (McConnachie 2012 / UNGC) shown as "<name> Dwarf".
DWARF_SUFFIX = {'Draco', 'Ursa Minor', 'Sculptor', 'Fornax', 'Carina', 'Sextans', 'Hercules', 'Phoenix',
                'Cetus', 'Tucana', 'Aquarius', 'Antlia', 'Coma Berenices', 'Ursa Major I', 'Ursa Major II'}
LOCAL_NAMES = {'Sagittarius dSph': 'Sagittarius Dwarf Spheroidal', 'Sag dSph': 'Sagittarius Dwarf Spheroidal',
               'Sagittarius dIrr': 'Sagittarius Dwarf Irregular', 'Sag dIr': 'Sagittarius Dwarf Irregular',
               'Pegasus dIrr': 'Pegasus Dwarf Irregular', 'UMin': 'Ursa Minor', 'Andromeda': 'M31',
               'Triangulum': 'M33', 'Maffei1': 'Maffei 1', 'Maffei2': 'Maffei 2'}

# Literature distances that supersede the catalogues (kpc). McConnachie's M31 sub-group (its
# satellites, M33, IC 10, ...) is moved with M31: their distances share M31's scale (783 kpc there).
DISTANCE_OVERRIDES = {
    'M31': 761.0,   # Li, Riess et al. 2021, ApJ 920, 84 (HST Cepheids, DM 24.407 +- 0.032)
    'LMC': 49.59,   # Pietrzynski et al. 2019, Nature 567, 200 (detached eclipsing binaries)
    'SMC': 62.44,   # Graczyk et al. 2020, ApJ 904, 13 (detached eclipsing binaries)
}
# Interacting pairs: two galaxies with their own redshift-independent distances that lie within
# COMPANION_ARCMIN and COMPANION_DV_KMS of each other (and within COMPANION_DM_MAG in distance modulus)
# are one system; the fainter takes the brighter's distance (NGC 5195 was 0.74 Mpc in front of M51).
COMPANION_ARCMIN = 10.0
COMPANION_DV_KMS = 150.0
COMPANION_DM_MAG = 0.75
# A local-catalogue position contradicted by both 2MASS and RC3 (see build_galaxies).
POS_AGREE_ARCSEC = 30.0
POS_OFF_ARCSEC = 60.0
# Disk inclinations where the isophotal axis ratio is misleading (large bulge, tidal arms).
INCLINATION_OVERRIDES = {
    'M104': 84.0,   # Sombrero: nearly edge-on dust lane (e.g. Emsellem 1995, A&A 303, 673)
    'M51': 22.0,    # Whirlpool: kinematic inclination (Colombo et al. 2014, ApJ 784, 3); the tidal arm
                    # towards NGC 5195 makes the isophotes look more inclined (53 deg from b/a)
}
# Stellar masses (M_sun) where the K-band estimate is far from the literature: 2MASS photometry
# misses much of M31's light, and M/L_K = 0.6 gave 3.1e10, below the Milky Way's 5e10.
STELLAR_MASS_OVERRIDES = {
    'M31': 1.0e11,  # Tamm et al. 2012, A&A 546, A4 (1.0-1.5e11); Sick et al. 2015, IAU Symp. 311, 82 (1.0e11)
}
# Morphology overrides (catalogue class code -> GalaxyType): the SMC is classified SB(s)m but is an
# irregular, elongated along the line of sight (e.g. Subramanian & Subramaniam 2012).
MORPH_OVERRIDES = {'SMC': 4}


def with_aliases(cid):
    """A canonical id plus its Messier/NGC equivalent."""
    out = {cid}
    m = re.match(r'^M(\d+)$', cid)
    if m and int(m.group(1)) in MESSIER_NGC:
        out.add(f'NGC{MESSIER_NGC[int(m.group(1))]}')
    m = re.match(r'^NGC(\d+)$', cid)
    if m and int(m.group(1)) in NGC_MESSIER:
        out.add(f'M{NGC_MESSIER[int(m.group(1))]}')
    return out

# Galaxy clusters: members selected by angle and heliocentric velocity are placed at the cluster
# distance (removing the "fingers of god" of redshift distances). Centres from NED.
#   name, RA, Dec, distance (Mpc, None = Hubble distance of the mean velocity), mean v_helio,
#   angular radius (deg), v range, line-of-sight half depth (Mpc)
CLUSTERS = [
    ('Virgo', 187.70593, 12.39112, 16.5, 1079.0, 6.0, (-700.0, 2700.0), 1.5),     # Mei et al. 2007 (SBF)
    ('Fornax', 54.62108, -35.45074, 20.0, 1454.0, 3.0, (600.0, 2400.0), 0.7),     # Blakeslee et al. 2009 (SBF)
    ('Coma', 194.95310, 27.98070, None, 6925.0, 1.7, (4000.0, 10000.0), 2.0),
    ('Perseus', 49.95067, 41.51170, None, 5366.0, 1.5, (2400.0, 8400.0), 1.8),
    ('Centaurus', 192.20521, -41.31111, None, 3400.0, 2.0, (1800.0, 5200.0), 1.2),
    ('Hydra', 159.17821, -27.52817, None, 3777.0, 1.5, (2000.0, 5600.0), 1.2),
    ('Norma', 243.76580, -60.90720, None, 4871.0, 1.5, (2500.0, 7300.0), 1.5),
    ('Leo (A1367)', 176.00900, 19.95000, None, 6495.0, 1.2, (4000.0, 9000.0), 1.5),
    ('Antlia', 157.50279, -35.32533, None, 2800.0, 1.5, (1500.0, 4100.0), 0.7),
]


def canon_id(s):
    """Canonical identifier: 'MESSIER_031' -> 'M31', 'NGC 0224' -> 'NGC224', 'ESO 270- 9' -> 'ESO270-9'."""
    s = s.strip().upper().replace('_', ' ')
    s = re.sub(r'\s+', ' ', s)
    m = re.match(r'^(MESSIER|M)\s*0*(\d+)\s*([AB]?)$', s)
    if m:
        if m.group(3) == 'B' and m.group(2) == '51':
            return 'NGC5195'          # M51b, the companion of the Whirlpool
        return 'M' + m.group(2)       # M51a is M51
    m = re.match(r'^(NGC|IC|UGC|UGCA|PGC|ARP|MRK|DDO|KK|KKH|KKR|KKS|AGC|CGCG|VV)\s*0*(\d+)\s*([A-Z]?)$', s)
    if m:
        return m.group(1) + m.group(2) + m.group(3)
    m = re.match(r'^ESO\s*0*(\d+)\s*-\s*[A-Z]?\s*0*(\d+)\s*([A-Z]?)$', s)
    if m:
        return f'ESO{m.group(1)}-{m.group(2)}{m.group(3)}'
    return s


PRETTY_PREFIX = {'NGC': 'NGC', 'IC': 'IC', 'UGC': 'UGC', 'UGCA': 'UGCA', 'PGC': 'PGC', 'ARP': 'Arp', 'MRK': 'Mrk',
                 'DDO': 'DDO', 'KK': 'KK', 'KKH': 'KKH', 'KKR': 'KKR', 'KKS': 'KKS', 'AGC': 'AGC', 'CGCG': 'CGCG', 'VV': 'VV'}


def pretty_id(cid):
    m = re.match(r'^M(\d+)$', cid)
    if m:
        return 'M' + m.group(1)
    m = re.match(r'^(UGCA|NGC|IC|UGC|PGC|ARP|MRK|DDO|KKH|KKR|KKS|KK|AGC|CGCG|VV)(\d+[A-Z]?)$', cid)
    if m:
        return PRETTY_PREFIX[m.group(1)] + ' ' + m.group(2)
    m = re.match(r'^ESO(\d+)-(\d+[A-Z]?)$', cid)
    if m:
        return f'ESO {m.group(1)}-{m.group(2)}'
    return None


NAME_RANK = {'M': 0, 'NGC': 1, 'IC': 2, 'UGC': 4, 'ESO': 5, 'ARP': 6, 'MRK': 7, 'UGCA': 8, 'DDO': 9}


def id_rank(cid):
    if re.match(r'^M\d+$', cid):
        return 0
    m = re.match(r'^([A-Z]+)', cid)
    return NAME_RANK.get(m.group(1), 50) if m else 50


def display_name(ids, local_name=None):
    """Display name from the identifiers of one galaxy (canonical forms) and a proper LG name."""
    ids = set(i for i in ids if i)
    ngc = [int(re.match(r'^NGC(\d+)$', i).group(1)) for i in ids if re.match(r'^NGC\d+$', i)]
    for n in ngc:
        if n in NGC_MESSIER:
            ids.add(f'M{NGC_MESSIER[n]}')
    messier = sorted(i for i in ids if re.match(r'^M\d+$', i))
    for i in messier:
        m = int(i[1:])
        if m in MESSIER_NGC:
            ids.add(f'NGC{MESSIER_NGC[m]}')
    for key in sorted(ids, key=lambda c: (id_rank(c), c)):
        if key in COMMON_NAMES:
            ident = pretty_id(key) if key != 'ESO97-13' else 'ESO 97-G13'
            return f'{COMMON_NAMES[key]} ({ident})' if ident else COMMON_NAMES[key]
    if local_name in COMMON_NAMES:
        return COMMON_NAMES[local_name]
    if messier:
        m = int(messier[0][1:])
        return f'M{m} (NGC {MESSIER_NGC[m]})' if m in MESSIER_NGC else f'M{m}'
    ranked = sorted((id_rank(c), c) for c in ids if pretty_id(c))
    if ranked and ranked[0][0] <= 2:      # NGC / IC
        return pretty_id(ranked[0][1])
    if local_name:
        return local_name
    if ranked and ranked[0][0] < 50:
        return pretty_id(ranked[0][1])
    return None


def local_display(name):
    """Readable form of a McConnachie / UNGC proper name ('Sextans (I)' -> 'Sextans Dwarf')."""
    name = name.strip()
    name = LOCAL_NAMES.get(name, name)
    m = re.match(r'^(.*) \(I\)$', name)
    if m:
        name = m.group(1) if m.group(1) in DWARF_SUFFIX else m.group(1) + ' I'
    m = re.match(r'^And ([IVXL]+)$', name)
    if m:
        name = 'Andromeda ' + m.group(1)
    m = re.match(r'^(Bootes|Leo|Pisces|Canes Venatici|Segue|Ursa Major)([IVX]+)$', name)
    if m:
        name = m.group(1) + ' ' + m.group(2)
    if name in DWARF_SUFFIX:
        name += ' Dwarf'
    return name


def comoving_distance_table():
    zs, ds = [0.0], [0.0]
    dz = 0.0005
    z, d = 0.0, 0.0
    e = lambda zz: math.sqrt(OMEGA_M * (1 + zz) ** 3 + (1 - OMEGA_M))
    while z < 0.6:
        d += dz / 6.0 * (1 / e(z) + 4 / e(z + dz / 2) + 1 / e(z + dz))
        z += dz
        zs.append(z)
        ds.append(d * C_KMS / H0)
    return zs, ds


ZTAB, DTAB = comoving_distance_table()


def comoving_mpc(v_kms):
    z = v_kms / C_KMS
    if z <= 0:
        return 0.0
    if z >= ZTAB[-1]:
        return DTAB[-1]
    j = bisect.bisect_right(ZTAB, z)
    t = (z - ZTAB[j - 1]) / (ZTAB[j] - ZTAB[j - 1])
    return DTAB[j - 1] + (DTAB[j] - DTAB[j - 1]) * t


def v_lg(vh, l, b):
    """Local Group frame (Karachentsev & Makarov 1996, as used by UNGC/NED)."""
    lr, br = math.radians(l), math.radians(b)
    return vh + 316.0 * (math.sin(br) * math.sin(math.radians(-4.0)) +
                         math.cos(br) * math.cos(math.radians(-4.0)) * math.cos(lr - math.radians(93.0)))


def v_cmb(vh, l, b):
    """CMB frame, Planck 2018 dipole: 369.82 km/s towards (l, b) = (264.021, 48.253)."""
    lr, br = math.radians(l), math.radians(b)
    la, ba = math.radians(264.021), math.radians(48.253)
    return vh + 369.82 * (math.sin(br) * math.sin(ba) + math.cos(br) * math.cos(ba) * math.cos(lr - la))


def flow_velocity(vh, l, b):
    """Local Group frame nearby, CMB frame far away, blended between 2500 and 4500 km/s."""
    a = v_lg(vh, l, b)
    w = min(max((a - 2500.0) / 2000.0, 0.0), 1.0)
    return (1.0 - w) * a + w * v_cmb(vh, l, b)


def color_bv(t):
    """Typical (B-V)_0 by T-type (Roberts & Haynes 1994)."""
    pts = [(-5, 0.95), (-2, 0.90), (1, 0.80), (2, 0.75), (3, 0.70), (4, 0.65), (5, 0.55), (6, 0.50), (7, 0.47),
           (8, 0.45), (9, 0.40), (11, 0.40)]
    return interp(pts, t)


def color_vk(t):
    """Typical V-K: Jarrett et al. (2003) <B-K> minus (B-V)."""
    if t <= 2:
        bk = 4.10
    elif t <= 8:
        bk = 4.60 - 0.25 * t
    else:
        bk = 2.35
    return bk - color_bv(t)


def interp(pts, x):
    if x <= pts[0][0]:
        return pts[0][1]
    for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
        if x <= x1:
            return y0 + (y1 - y0) * (x - x0) / (x1 - x0)
    return pts[-1][1]


def mass_to_light_k(t):
    """K-band stellar M/L (McGaugh & Schombert 2014 for disks; older populations heavier)."""
    return 0.8 if t < 0.5 else (0.5 if t >= 8.5 else 0.6)


def hubble_type_from_rc3(typ):
    """Bar family from an RC3 type string such as '.SBS4P.'."""
    m = re.match(r'^[.P]?([ESLI])([ABX])?', typ.strip())
    if not m:
        return None
    return m.group(2)


def mcc_morphology(mtype):
    """(T, barred, dSph) from McConnachie's morphological type."""
    s = mtype.strip()
    barred = 'SB' in s or '(B)' in s
    if s.startswith('dSph') or s == 'dE/dSph':
        return (-3.0 if s.startswith('dSph') else -5.0), barred, True
    if s.startswith('dE') or s.startswith('E'):
        return -5.0, barred, False
    if s.startswith('cE'):
        return -6.0, barred, False
    if 'Irr' in s:
        return 10.0, barred, False
    m = re.match(r'^S\(?B?\)?\s*\(?[rs]?\)?\s*([a-d]+)', s)
    stage = {'a': 1.0, 'ab': 2.0, 'b': 3.0, 'bc': 4.0, 'c': 5.0, 'cd': 6.0, 'd': 7.0}
    if m and m.group(1) in stage:
        return stage[m.group(1)], barred, False
    if s.startswith('S0'):
        return -2.0, barred, False
    return None, barred, False


def inclination_deg(q, t, dsph):
    """Disk inclination from the axis ratio (Hubble 1926) with intrinsic thickness q0 by type."""
    if dsph or t <= -3.5:
        return 90.0
    q0 = 0.35 if t >= 8.5 else (0.25 if t < 0.5 else 0.20)
    q = min(max(q, q0), 1.0)
    c2 = (q * q - q0 * q0) / (1.0 - q0 * q0)
    return math.degrees(math.acos(math.sqrt(max(c2, 0.0))))


def read_fixed(path, opener=open):
    with opener(path, 'rt', encoding='latin-1') as f:
        return [l.rstrip('\n') for l in f if l.strip()]


def col(line, a, b):
    """Bytes a..b (1-based, inclusive) of a fixed-width line."""
    return line[a - 1:b]


def sexagesimal(h, m, s, sign='+'):
    v = abs(float(h)) + float(m) / 60.0 + float(s) / 3600.0
    return -v if sign.strip() == '-' else v


# Holmberg diameters behind the Milky Way's dust. UNGC table 1 gives the observed angular a26; table 2
# gives the linear A26 corrected for Galactic extinction and inclination (Fouque & Paturel 1985; in
# the catalogue log A26/a26 is close to 0.094 A_B up to A_B = 8 mag, minus <= 0.04 dex for edge-on
# disks). The correction is used as published up to A_B = UNGC_AB_TRUSTED; beyond, where the
# linear law is extrapolated (HIZSS012 at A_B = 7.9 would grow 5.4x to 36 kpc), it is limited to its
# value at that extinction, unless the 2MASS (K band, ~11x less extinction) size is larger.
FP85_DEX_PER_MAG = 0.094
UNGC_AB_TRUSTED = 5.0
UNGC_A26_PRECISE_KPC = 0.5   # table 2 lists A26 to 0.01 kpc: below this, apply the law to a26 instead


def load_ungc(p1, p2, p6):
    t1 = read_fixed(p1)
    t2 = {col(l, 1, 18).strip(): l for l in read_fixed(p2)}
    t6 = {col(l, 1, 18).strip(): l for l in read_fixed(p6)}
    out = []
    for l in t1:
        name = col(l, 1, 18).strip()
        ra = sexagesimal(col(l, 20, 21), col(l, 23, 24), col(l, 26, 29)) * 15.0
        dec = sexagesimal(col(l, 32, 33), col(l, 35, 36), col(l, 38, 39), col(l, 31, 31))
        g = {'name': name, 'ra': ra, 'dec': dec, 'a26': fnum(col(l, 41, 46)), 'ba': fnum(col(l, 48, 51)),
             'ab': fnum(col(l, 53, 56)), 'bmag': fnum(col(l, 66, 70)), 'kmag': fnum(col(l, 79, 83)),
             'tt': fnum(col(l, 99, 100)), 'mcl': col(l, 102, 106).strip(), 'hrv': fnum(col(l, 110, 113)),
             'dist': fnum(col(l, 115, 119)), 'method': col(l, 121, 124).strip()}
        l2 = t2.get(name)
        if l2:
            g.update({'A26': fnum(col(l2, 41, 45)), 'incl': fnum(col(l2, 47, 48)), 'BMag': fnum(col(l2, 59, 63)),
                      'klum': fnum(col(l2, 70, 74))})
        # Extinction-corrected angular diameter (arcmin): table 2's A26 at the table 1 distance it
        # was computed with, or the same law applied to a26 where A26 is too coarse or missing.
        a26, ab, dist = g['a26'], g['ab'] or 0.0, g['dist']
        if a26:
            A26 = g.get('A26')
            if A26 and A26 >= UNGC_A26_PRECISE_KPC and dist:
                g['a26_corr'] = math.degrees(A26 / (dist * 1e3)) * 60.0
            else:
                g['a26_corr'] = a26 * 10.0 ** (FP85_DEX_PER_MAG * ab)
        l6 = t6.get(name)
        if l6:
            dm = fnum(col(l6, 20, 24))
            if dm is not None:
                g['dm'] = dm
                g['method'] = col(l6, 32, 35).strip() or g['method']
        out.append(g)
    return out


def load_mcconnachie(p1, p2, p3, p4):
    def rows(p):
        return [[c.strip() for c in l.split('|')] for l in read_fixed(p)]
    t1, t2, t3, t4 = rows(p1), rows(p2), rows(p3), rows(p4)
    if not (len(t1) == len(t2) == len(t3) == len(t4)):
        raise ValueError('McConnachie tables have different lengths')
    if min(len(r) for r in t1) < 13 or min(len(r) for r in t2) < 14 or min(len(r) for r in t3) < 25:
        raise ValueError('McConnachie tables have fewer columns than expected')

    def norm(s):
        return re.sub(r'[^a-z0-9]', '', s.lower().replace('(i)', ''))
    out = []
    for r1, r2, r3, r4 in zip(t1, t2, t3, t4):
        n1, n2, n3, n4 = r1[1], r2[0], r3[0], r4[0]
        key = norm(n3)[:5]
        if not (norm(n1).startswith(key) and norm(n2).startswith(key) and norm(n4).startswith(key)):
            raise ValueError(f'McConnachie tables are not aligned: {n1!r} {n2!r} {n3!r} {n4!r}')
        if not r1[6]:
            continue
        ra = sexagesimal(r1[6], r1[7], r1[8]) * 15.0
        dec = sexagesimal(r1[10], r1[11], r1[12], r1[9])
        t, barred, dsph = mcc_morphology(r1[5])
        # table3 fields (split on '|'): 2 Vmag, 6 R1 (arcmin), 14 PA, 17 Ell, 20 M_V, 24 R2 (pc)
        g = {'name': n1, 'subgroup': r1[0], 'oname': r1[2], 'mtype': r1[5], 'ra': ra, 'dec': dec, 'dm': fnum(r2[5]),
             'hrv': fnum(r2[13]), 'vmag': fnum(r3[2]), 'rh_arcmin': fnum(r3[6]), 'pa': fnum(r3[14]),
             'ell': fnum(r3[17]), 'VMag': fnum(r3[20]), 'rh_pc': fnum(r3[24]),
             'mass6': fnum(r4[1]), 't': t, 'barred': barred, 'dsph': dsph}
        out.append(g)
    return out


def load_2mrs(path):
    out = []
    for l in read_fixed(path, gzip.open):
        cat = col(l, 206, 233).strip()
        typ = col(l, 165, 169)
        tt = fnum(typ[0:2])
        out.append({'id': col(l, 1, 16).strip(), 'ra': float(col(l, 18, 26)), 'dec': float(col(l, 28, 36)),
                    'l': float(col(l, 38, 46)), 'b': float(col(l, 48, 56)), 'kc': fnum(col(l, 58, 63)),
                    'kt': fnum(col(l, 79, 84)), 'riso': fnum(col(l, 142, 146)), 'rext': fnum(col(l, 148, 152)),
                    'ba': fnum(col(l, 154, 158)), 'lga': col(l, 160, 160) == 'Z', 'tt': tt,
                    'bar': typ[2:3].strip(), 'cz': fnum(col(l, 174, 178)), 'cat': cat})
    return out


def load_xsc(path):
    header, rows = tsv_data_lines(path)
    ix = {h: i for i, h in enumerate(header)}
    out = {}
    for r in rows:
        p = r.split('\t')
        if len(p) < len(header):
            continue
        out[p[ix['2MASX']].strip()] = {'ba': fnum(p[ix['Sb/a']]), 'pa': fnum(p[ix['Spa']]),
                                       'rext': fnum(p[ix['r.ext']]), 'k': fnum(p[ix['K.K20e']]),
                                       'ra': fnum(p[ix['RAJ2000']]), 'dec': fnum(p[ix['DEJ2000']])}
    return out


def xsc_nearest(xsc, targets, radius_arcmin):
    """2MASS XSC total radius (arcmin) of the source nearest to each target (dict with 'u', 'ra', 'dec')
    within radius_arcmin(target), in one pass over the catalogue. Returns {id(target): rext_arcmin}."""
    bands = {}                              # 1-degree declination bands -> targets that reach them
    for t in targets:
        r = radius_arcmin(t) / 60.0
        for band in range(math.floor(t['dec'] - r), math.floor(t['dec'] + r) + 1):
            bands.setdefault(band, []).append(t)
    best = {}
    for x in xsc.values():
        if x['ra'] is None or x['dec'] is None or x['rext'] is None:
            continue
        for t in bands.get(math.floor(x['dec']), ()):
            sep = ang_sep_deg(t['u'], unit_vector(x['ra'], x['dec'])) * 60.0
            if sep <= radius_arcmin(t) and (id(t) not in best or sep < best[id(t)][0]):
                best[id(t)] = (sep, x['rext'] / 60.0)
    return {k: v[1] for k, v in best.items()}


def load_rc3(path):
    header, rows = tsv_data_lines(path)
    ix = {h: i for i, h in enumerate(header)}
    out = []
    for r in rows:
        p = r.split('\t')
        if len(p) < len(header):
            continue
        rs, ds = p[ix['RA2000']].split(), p[ix['DE2000']].split()
        if len(rs) != 3 or len(ds) != 3:
            continue
        sign = '-' if ds[0].startswith('-') else '+'
        ra = sexagesimal(rs[0], rs[1], rs[2]) * 15.0
        dec = sexagesimal(ds[0].lstrip('+-'), ds[1], ds[2], sign)
        d25 = fnum(p[ix['D25']])
        r25 = fnum(p[ix['R25']])
        ids = [canon_id(p[ix[k]]) for k in ('name', 'altname', 'desig', 'PGC') if p[ix[k]].strip()]
        aliases = set()
        for i in ids:
            aliases |= with_aliases(i)
        pgc = re.match(r'^PGC(\d+)$', canon_id(p[ix['PGC']])) if p[ix['PGC']].strip() else None
        out.append({'ra': ra, 'dec': dec, 'ids': ids, 'aliases': aliases, 'pgc': int(pgc.group(1)) if pgc else 0,
                    'type': p[ix['type']], 't': fnum(p[ix['T']]),
                    'd25': 10.0 ** d25 * 0.1 if d25 is not None else None,       # arcmin
                    'q': 10.0 ** (-r25) if r25 is not None else None,
                    'pa': fnum(p[ix['PA']]), 'bt': fnum(p[ix['BT']])})
    return out


def best_match(index, u, radius_deg, accept=None):
    best = None
    for sep, p in index.query(u, radius_deg):
        if accept and not accept(p):
            continue
        if best is None or sep < best[0]:
            best = (sep, p)
    return best


def build_galaxies(ungc, mcc, twomrs, xsc, rc3, stats):
    galaxies = []
    # ---- McConnachie (2012): Local Group and neighbours within 3 Mpc.
    for m in mcc:
        if m['name'] in ('The Galaxy', 'Canis Major'):     # the host itself; a disputed disk overdensity
            continue
        if m['dm'] is None:
            continue
        g = new_galaxy(m['ra'], m['dec'])
        g['src'] |= G_MCC
        g['dist_kpc'] = 10.0 ** (m['dm'] / 5.0 + 1.0) / 1e3
        g['mcc_dist_kpc'] = g['dist_kpc']
        g['mcc_subgroup'] = m['subgroup']
        g['dist_method'] = DIST_DIRECT
        g['local_name'] = local_display(m['name'])
        for x in m['oname'].split(',') + [m['name']]:
            if x.strip():
                g['ids'] |= with_aliases(canon_id(x))
        g['own_ids'] |= g['ids']
        g['t_mcc'] = m['t']
        g['barred'] = m['barred']
        g['dsph'] = m['dsph']
        if m['VMag'] is not None:
            g['MV_mcc'] = m['VMag']
        if m['pa'] is not None:
            g['pa_mcc'] = m['pa']
        if m['ell'] is not None:
            g['q_mcc'] = max(0.05, 1.0 - m['ell'])
        if m['rh_pc'] is not None and m['rh_pc'] > 0:
            g['reff_mcc_kpc'] = m['rh_pc'] / 1e3
        if m['mass6'] is not None and m['mass6'] > 0:
            g['logm_mcc'] = math.log10(m['mass6'] * 1e6)
        if m['hrv'] is not None:
            g['cz'] = m['hrv']
        g['seed_key'] = 'McC:' + m['name']
        galaxies.append(g)
    mcc_index = SkyIndex(1.0)
    for g in galaxies:
        mcc_index.add(g['u'], g)

    # ---- UNGC (Karachentsev et al. 2013): Local Volume within ~11 Mpc.
    n_ungc_merged = 0
    for k in ungc:
        if k['name'] == 'Milky Way':
            continue
        u = unit_vector(k['ra'], k['dec'])
        size = k['a26'] or 1.0
        cid = canon_id(k['name'])
        aliases = with_aliases(cid)
        lname = local_display(k['name'])
        hit = best_match(mcc_index, u, max(2.0 / 60.0, 0.08 * size / 60.0),
                         lambda g: not (g['src'] & G_UNGC))
        if hit is None:
            for g in galaxies:   # name match catches large galaxies with offset centres
                if g['src'] & G_MCC and not g['src'] & G_UNGC and (aliases & g['ids'] or lname == g['local_name']):
                    if ang_sep_deg(u, g['u']) < 0.5:
                        hit = (0.0, g)
                        break
        d_ungc = 10.0 ** (k['dm'] / 5.0 + 1.0) / 1e3 if 'dm' in k else (k['dist'] or 1.0) * 1e3
        if hit is not None:
            g = hit[1]
            n_ungc_merged += 1
        else:
            g = new_galaxy(k['ra'], k['dec'])
            g['dist_kpc'] = d_ungc
            g['dist_method'] = DIST_DIRECT
            g['dsph'] = k['mcl'] == 'Sph'
            if k['hrv'] is not None:
                g['cz'] = k['hrv']
            g['seed_key'] = 'UNGC:' + k['name']
            galaxies.append(g)
        g['src'] |= G_UNGC
        g['ungc_dist_kpc'] = d_ungc
        g['ids'] |= aliases
        g['own_ids'] |= aliases
        g['ungc_name'] = pretty_id(cid) or lname
        if not g.get('local_name') and not pretty_id(cid):
            g['local_name'] = lname
        t = k['tt']
        if k['mcl'] in ('dE', 'dEem') and (t is None or t <= -1):
            t = -5.0
        g['t_ungc'] = t
        if k['a26']:
            g['a26_arcmin'] = k['a26']                   # observed: used for matching apertures
            g['a26_corr_arcmin'] = k['a26_corr']         # corrected for Galactic extinction
            g['ab_ungc'] = k['ab'] or 0.0
        if k['ba']:
            g['q_ungc'] = k['ba']
        if k.get('incl') is not None:
            g['incl_ungc'] = k['incl']
        if k.get('BMag') is not None:
            g['MB_ungc'] = k['BMag']
        if k.get('klum') is not None:
            g['logLK_ungc'] = k['klum']
        if k['kmag'] is not None:
            g['kmag_ungc'] = k['kmag']
    stats['ungc_merged_with_mcconnachie'] = n_ungc_merged
    # 2MASS sizes for the most obscured Local Volume galaxies (most lie at |b| < 5 deg, outside 2MRS).
    obscured = [g for g in galaxies if g.get('ab_ungc', 0.0) > UNGC_AB_TRUSTED]
    rext = xsc_nearest(xsc, obscured, lambda g: max(1.0, 0.1 * g['a26_arcmin']))
    for g in obscured:
        if id(g) in rext:
            g['xsc_rext_arcmin'] = rext[id(g)]

    local_index = SkyIndex(1.0)
    for g in galaxies:
        local_index.add(g['u'], g)

    # ---- 2MRS (Huchra et al. 2012) with 2MASS XSC position angles.
    n_2mrs_merged = 0
    for t in twomrs:
        u = unit_vector(t['ra'], t['dec'])
        rext_arcmin = (10.0 ** t['rext']) / 60.0 if t['rext'] is not None else None

        def consistent(g, sep, t=t):
            if g['src'] & G_2MRS:
                return False
            if g.get('cz') is not None and t['cz'] is not None and abs(g['cz'] - t['cz']) > 400.0:
                return False
            # UNGC K magnitudes of dwarfs are often estimated from B: only check them for loose matches.
            if sep > 0.5 / 60.0 and g.get('kmag_ungc') is not None and t['kt'] is not None and \
                    abs(g['kmag_ungc'] - t['kt']) > 2.0:
                return False
            return True
        size = max(2.0 * (rext_arcmin or 0.5), 1.0)
        hit = None
        for sep, g in local_index.query(u, max(1.5 / 60.0, 0.1 * size / 60.0)):
            if consistent(g, sep) and (hit is None or sep < hit[0]):
                hit = (sep, g)
        cat_ids = with_aliases(canon_id(t['cat'])) if t['cat'] else set()
        if hit is None and cat_ids:
            # big local galaxies with offset centres: match by catalogue name (MESSIER_051 / NGC5194)
            for sep, g in local_index.query(u, 0.5):
                if cat_ids & g['ids'] and consistent(g, 0.0):
                    hit = (sep, g)
                    break
        if hit is not None:
            g = hit[1]
            g['u_2mrs'] = u
            n_2mrs_merged += 1
        else:
            g = new_galaxy(t['ra'], t['dec'])
            galaxies.append(g)
        g['src'] |= G_2MRS
        g['seed_key'] = '2MASX J' + t['id']
        g['lb'] = (t['l'], t['b'])
        g['ids'] |= cat_ids
        g['own_ids'] |= cat_ids
        if t['kt'] is not None:
            g['kt'] = t['kt']
        if t['cz'] is not None:
            g['cz_2mrs'] = t['cz']
            if g.get('cz') is None:
                g['cz'] = t['cz']
        if t['tt'] is not None:
            g['t_2mrs'] = t['tt']
            if t['bar'] in ('B', 'X'):
                g['bar_2mrs'] = t['bar']
        x = xsc.get(t['id'])
        if x is not None and x['pa'] is not None:
            g['pa_xsc'] = x['pa']
        if t['ba'] is not None:
            g['q_2mass'] = t['ba']
        if rext_arcmin is not None:
            g['rext_arcmin'] = rext_arcmin
    stats['2mrs_merged_with_local'] = n_2mrs_merged

    # ---- RC3 attributes (optical D25, axis ratio, position angle, type, names).
    rc3_index = SkyIndex(0.5)
    for r in rc3:
        rc3_index.add(unit_vector(r['ra'], r['dec']), r)
    used = set()
    n_rc3 = 0
    for g in sorted(galaxies, key=lambda g: (g.get('kt', 99.0), g['ra'], g['dec'])):
        size_arcmin = g.get('a26_arcmin') or (2.0 * g['rext_arcmin'] if g.get('rext_arcmin') else 1.0)

        def ok(r, g=g):
            if id(r) in used:
                return False
            if r['bt'] is not None and g.get('kt') is not None and not (0.0 < r['bt'] - g['kt'] < 7.0):
                return False
            return True
        hit = best_match(rc3_index, g['u'], max(0.75 / 60.0, 0.08 * size_arcmin / 60.0), ok)
        if hit is None and g['ids']:
            for sep, r in rc3_index.query(g['u'], 0.5):
                if ok(r) and r['aliases'] & g['ids']:
                    hit = (sep, r)
                    break
        if hit is None:
            continue
        r = hit[1]
        used.add(id(r))
        n_rc3 += 1
        g['src'] |= G_RC3
        g['u_rc3'] = unit_vector(r['ra'], r['dec'])
        g['ids'] |= r['aliases']
        if r['d25']:
            g['d25_rc3'] = r['d25']
        if r['q'] is not None:
            g['q_rc3'] = r['q']
        if r['pa'] is not None:
            g['pa_rc3'] = r['pa']
        if r['t'] is not None:
            g['t_rc3'] = r['t']
        bar = hubble_type_from_rc3(r['type'])
        if bar in ('B', 'X'):
            g['bar_rc3'] = bar
    stats['rc3_matches'] = n_rc3

    # ---- Positions. Galaxies keep the position of the catalogue that created them (McConnachie, UNGC,
    # 2MRS), unless 2MASS and RC3 agree (within POS_AGREE_ARCSEC) on a centre more than POS_OFF_ARCSEC
    # from it: UNGC puts NGC 5194 (M51) 2.4' north of its nucleus, half way to NGC 5195.
    corrected = []
    for g in galaxies:
        u2, ur = g.get('u_2mrs'), g.get('u_rc3')
        if u2 is None or ur is None or ang_sep_deg(u2, ur) * 3600.0 > POS_AGREE_ARCSEC:
            continue
        off = min(ang_sep_deg(g['u'], u2), ang_sep_deg(g['u'], ur)) * 3600.0
        if off > POS_OFF_ARCSEC:
            label = g.get('ungc_name') or g.get('local_name') or g['seed_key']
            corrected.append(f"{label} {off / 60.0:.1f}'")
            g['u'] = u2
            g['ra'], g['dec'] = ra_dec(u2)
    stats['positions_corrected'] = sorted(corrected)

    # ---- Size calibration: everything is converted to an optical D25-equivalent diameter. RC3's D25
    # is not corrected for extinction, so the Holmberg/D25 ratio is measured where A_B < 0.3 mag.
    k_h = median([math.log10(g['a26_corr_arcmin'] / g['d25_rc3']) for g in galaxies
                  if g.get('a26_arcmin') and g.get('d25_rc3') and g['ab_ungc'] < 0.3])
    k_x = median([math.log10(g['d25_rc3'] / (2.0 * g['rext_arcmin'])) for g in galaxies
                  if g.get('rext_arcmin') and g.get('d25_rc3') and not (g['src'] & (G_UNGC | G_MCC))])
    stats['calib_log_a26_over_d25'] = round(k_h, 4)
    stats['calib_log_d25_over_2rext'] = round(k_x, 4)

    # ---- Distances for galaxies without redshift-independent ones.
    clusters = []
    for (name, ra, dec, dist, vmean, rad, (vlo, vhi), depth) in CLUSTERS:
        u = unit_vector(ra, dec)
        l, b = galactic_lb(u)
        d = dist if dist is not None else comoving_mpc(flow_velocity(vmean, l, b))
        clusters.append({'name': name, 'u': u, 'dist': d, 'rad': rad, 'vlo': vlo, 'vhi': vhi, 'depth': depth,
                         'members': 0})
    # Typical luminosity for galaxies without a redshift (median M_K of the survey beyond 3000 km/s).
    mk_all = [g['kt'] - 5.0 * math.log10(comoving_mpc(g['cz']) * 1e6 / 10.0) for g in galaxies
              if g['dist_kpc'] is None and g.get('cz') is not None and g.get('kt') is not None and g['cz'] > 3000]
    mk_typ = median(mk_all) if mk_all else -23.5
    stats['typical_MK_for_photometric_distances'] = round(mk_typ, 3)
    n_low = 0
    for g in galaxies:
        g['seed'] = stable_seed(g['seed_key'])
        if g['dist_kpc'] is not None:
            continue
        cz = g.get('cz')
        if cz is None:
            d = 10.0 ** ((g.get('kt', 11.0) - mk_typ - 25.0) / 5.0)
            g['dist_kpc'] = min(max(d, 10.0), 300.0) * 1e3
            g['dist_method'] = DIST_PHOTOMETRIC
            continue
        member = None
        for c in clusters:
            if c['vlo'] <= cz <= c['vhi'] and ang_sep_deg(g['u'], c['u']) <= c['rad']:
                member = c
                break
        if member is not None:
            member['members'] += 1
            offset = (hash01(g['seed'], 11) + hash01(g['seed'], 12) - 1.0) * member['depth']
            g['dist_kpc'] = (member['dist'] + offset) * 1e3
            g['dist_method'] = DIST_CLUSTER
            g['src'] |= G_CLUSTER
            continue
        l, b = g.get('lb') or galactic_lb(g['u'])
        v = flow_velocity(cz, l, b)
        if v < 300.0:
            n_low += 1
        g['dist_kpc'] = comoving_mpc(max(v, 300.0)) * 1e3
        g['dist_method'] = DIST_HUBBLE
    stats['hubble_velocity_below_300'] = n_low
    stats['clusters'] = {c['name']: {'members': c['members'], 'distance_mpc': round(c['dist'], 2)} for c in clusters}

    # ---- Literature distances, and keeping close systems together.
    m31_scale = None
    for g in galaxies:
        override = None
        for cid in sorted(g['ids']):
            if cid in DISTANCE_OVERRIDES:
                override = cid
        if override is None and g.get('local_name') in DISTANCE_OVERRIDES:
            override = g['local_name']
        if override is not None:
            if override == 'M31' and g.get('mcc_dist_kpc'):
                m31_scale = DISTANCE_OVERRIDES['M31'] / g['mcc_dist_kpc']
            g['dist_kpc'] = DISTANCE_OVERRIDES[override]
            g['dist_method'] = DIST_LITERATURE
    n_m31 = 0
    if m31_scale is not None:
        for g in galaxies:
            if g.get('mcc_subgroup') == 'M31' and g['dist_method'] == DIST_DIRECT:
                g['dist_kpc'] *= m31_scale
                n_m31 += 1
    stats['m31_subgroup_rescaled'] = {'members': n_m31, 'factor': round(m31_scale or 1.0, 5)}
    stats['companions_moved'] = move_companions(galaxies)

    # ---- Derived quantities.
    out = []
    n_ab_limited = n_ext_corrected = 0
    for g in galaxies:
        ids = g['ids']
        flags = g['src']
        dist_kpc = g['dist_kpc']
        dist_mpc = dist_kpc / 1e3

        def rescale_mag(ref_kpc):     # magnitudes derived at another distance
            return 5.0 * math.log10(dist_kpc / ref_kpc) if ref_kpc else 0.0

        # Morphology: RC3, then 2MRS, then UNGC, then McConnachie, then a statistical guess.
        t = None
        for v in (g.get('t_rc3'), g.get('t_2mrs'), g.get('t_ungc'), g.get('t_mcc')):
            if v is None:
                continue
            if v == 90.0:          # RC3 "I0" (non-Magellanic irregular, e.g. M82)
                t = 10.0
                break
            if -7 <= v <= 11:
                t = float(v)
                break
            if v == -9:            # QSO/AGN host
                t = -5.0
                break
            if v == 12:            # HI cloud without a visible galaxy: a faint irregular
                t = 10.0
                break
        bar = g.get('bar_rc3') or g.get('bar_2mrs')
        if t is None:
            h = hash01(g['seed'], 4)
            if g.get('t_2mrs') == 20:                 # "S...": unclassified spiral
                t = float(int(1 + 7 * h))
            elif g.get('t_2mrs') in (15, 16):         # peculiar / Irr II
                t = 10.0
            else:
                pe, ps0, ps = (0.35, 0.35, 0.27) if flags & G_CLUSTER else (0.15, 0.20, 0.60)
                if h < pe:
                    t = -5.0
                elif h < pe + ps0:
                    t = -2.0
                elif h < pe + ps0 + ps:
                    t = float(int(1 + 7 * hash01(g['seed'], 5)))
                else:
                    t = 10.0
            if 0.5 <= t < 8.5 and bar is None and hash01(g['seed'], 6) < 0.35:
                bar = 'B'
            flags |= G_TYPE_GUESS
        dsph = bool(g.get('dsph'))
        if dsph:
            flags |= G_DSPH
        if bar == 'B' or g.get('barred'):
            flags |= G_BARRED
        elif bar == 'X':
            flags |= G_BARRED | G_WEAKBAR
        if dsph or t <= -3.5:
            morph = MORPH_ELLIPTICAL
        elif t < 0.5:
            morph = MORPH_LENTICULAR
        elif t < 8.5:
            morph = MORPH_BARRED if flags & G_BARRED else MORPH_SPIRAL
        elif t < 9.5 and flags & G_BARRED and not flags & G_WEAKBAR:
            morph = MORPH_BARRED            # barred Magellanic spirals such as the LMC
        else:
            morph = MORPH_IRREGULAR
        for cid in sorted(ids):
            if cid in MORPH_OVERRIDES:
                morph = MORPH_OVERRIDES[cid]

        # Size: optical D25-equivalent major diameter (kpc) and half-light radius where measured.
        # UNGC Holmberg diameters come first, corrected for Galactic extinction (table 2), which
        # matters for obscured galaxies such as Maffei 1 and 2, IC 342 and Circinus. RC3 D25 and
        # 2MASS sizes are as observed.
        reff_kpc = 0.0
        if g.get('reff_mcc_kpc'):
            reff_kpc = g['reff_mcc_kpc'] * dist_kpc / g['mcc_dist_kpc']
        if g.get('a26_arcmin'):
            a26 = g['a26_corr_arcmin']
            if g['ab_ungc'] > UNGC_AB_TRUSTED:
                lim = g['a26_arcmin'] * 10.0 ** (FP85_DEX_PER_MAG * UNGC_AB_TRUSTED)
                rext = g.get('rext_arcmin') or g.get('xsc_rext_arcmin')
                if rext:
                    lim = max(lim, 2.0 * rext * 10.0 ** (k_x + k_h))     # 2MASS size in Holmberg units
                if lim < a26:
                    a26 = lim
                    n_ab_limited += 1
            if a26 > 1.3 * g['a26_arcmin']:
                n_ext_corrected += 1
            d_arcmin = a26 / 10.0 ** k_h
        elif g.get('d25_rc3'):
            d_arcmin = g['d25_rc3']
        elif g.get('rext_arcmin'):
            d_arcmin = 2.0 * g['rext_arcmin'] * 10.0 ** k_x
        else:
            d_arcmin = None
        if d_arcmin is not None:
            diam_kpc = math.radians(d_arcmin / 60.0) * dist_kpc
        elif reff_kpc > 0.0:
            diam_kpc = 5.0 * reff_kpc
        else:
            diam_kpc = 1.0
        if dsph and reff_kpc > 0.0:
            diam_kpc = max(diam_kpc, 5.0 * reff_kpc)

        # Shape and orientation (axis ratio: McConnachie, RC3, UNGC, 2MASS).
        q = None
        for v in (g.get('q_mcc'), g.get('q_rc3'), g.get('q_ungc'), g.get('q_2mass')):
            if v is not None and v > 0.0:
                q = v
                break
        q = min(max(q if q is not None else 0.7, 0.05), 1.0)
        pa = None
        for v in (g.get('pa_mcc'), g.get('pa_rc3'), g.get('pa_xsc')):
            if v is not None:
                pa = v
                break
        if pa is not None:
            flags |= G_PA
            pa = pa % 180.0
        else:
            pa = 180.0 * hash01(g['seed'], 7)
        if g.get('incl_ungc') is not None and morph in (MORPH_SPIRAL, MORPH_BARRED, MORPH_LENTICULAR) \
                and g.get('q_mcc') is None:
            incl = g['incl_ungc']
        else:
            incl = inclination_deg(q, t, dsph)
        for cid in sorted(ids):
            if cid in INCLINATION_OVERRIDES:
                incl = INCLINATION_OVERRIDES[cid]

        # Luminosity (V) and stellar mass.
        z = max(g.get('cz') or 0.0, 0.0) / C_KMS if g['dist_method'] in (DIST_HUBBLE, DIST_CLUSTER) else 0.0
        mk = None
        if g.get('kt') is not None:
            dl_pc = dist_kpc * 1e3 * (1.0 + z)
            mk = g['kt'] - 5.0 * math.log10(dl_pc / 10.0) + 6.0 * math.log10(1.0 + z)   # k(z) = -6 log(1+z)
        if g.get('MV_mcc') is not None:
            mv = g['MV_mcc'] + rescale_mag(g['mcc_dist_kpc'])
        elif g.get('MB_ungc') is not None:
            mv = g['MB_ungc'] - color_bv(t) + rescale_mag(g['ungc_dist_kpc'])
        elif mk is not None:
            mv = mk + color_vk(t)
        else:
            mv = -14.0
        if g.get('logm_mcc') is not None:
            logm = g['logm_mcc'] + 0.4 * rescale_mag(g['mcc_dist_kpc'])
        elif g.get('logLK_ungc') is not None:
            logm = g['logLK_ungc'] + math.log10(mass_to_light_k(t)) + 0.4 * rescale_mag(g['ungc_dist_kpc'])
        elif mk is not None:
            logm = -0.4 * (mk - 3.28) + math.log10(mass_to_light_k(t))
        else:
            logm = -0.4 * (mv - 4.83) + 0.2         # M/L_V ~ 1.6
        for cid in sorted(ids):
            if cid in STELLAR_MASS_OVERRIDES:
                logm = math.log10(STELLAR_MASS_OVERRIDES[cid])
        cz = g.get('cz_2mrs', g.get('cz'))
        if cz is not None:
            flags |= G_CZ
        u = g['u']
        name = display_name(ids, g.get('local_name'))
        if name is None and flags & (G_UNGC | G_MCC):
            name = g.get('ungc_name') or g.get('local_name')
        out.append({'pos': (u[0] * dist_mpc, u[1] * dist_mpc, u[2] * dist_mpc), 'diam_kpc': diam_kpc,
                    'reff_kpc': reff_kpc, 'q': q, 'pa': pa, 'incl': incl, 'MV': mv, 'logm': logm,
                    'cz': cz if cz is not None else 0.0, 't': t, 'flags': flags, 'morph': morph,
                    'dist_method': g['dist_method'], 'seed': g['seed'], 'name': name, 'dist_mpc': dist_mpc,
                    'ids': ids, 'own_ids': g['own_ids'], 'kt': g.get('kt', 99.0)})
    stats['ungc_sizes_extinction_corrected_over_1.3x'] = n_ext_corrected
    stats['ungc_sizes_correction_limited'] = n_ab_limited
    # Unique display names. Distinct galaxies can share a name through ambiguous cross-identifications
    # (e.g. NGC 6548/6549/6550) or interacting pairs listed twice. The galaxy whose own catalogue entry
    # carries the identifier keeps the name (then the brightest); the others get their best identifier
    # that no other member of the group has, else a running number.
    groups = {}
    for i, g in enumerate(out):
        if g['name']:
            groups.setdefault(g['name'], []).append(i)
    n_renamed = 0
    for name, idxs in sorted(groups.items()):
        if len(idxs) < 2:
            continue
        key_ids = {c for c in out[idxs[0]]['ids'] if pretty_id(c) and pretty_id(c) in name}

        def owner_rank(i):
            g = out[i]
            return (0 if g['own_ids'] & key_ids else 1, g['kt'], g['dist_mpc'], g['seed'])
        idxs = sorted(idxs, key=owner_rank)
        for i in idxs[1:]:
            others = set()
            for j in idxs:
                if j != i:
                    others |= out[j]['ids']
            unique = sorted((id_rank(c), c) for c in out[i]['ids'] - others if pretty_id(c))
            n_renamed += 1
            if unique:
                out[i]['name'] = pretty_id(unique[0][1])
            else:
                out[i]['name'] = f'{name} #{idxs.index(i) + 1}'
    # A renamed galaxy may now collide with another name; fall back to running numbers.
    seen = {}
    for g in sorted(out, key=lambda g: (g['dist_mpc'], g['seed'])):
        n = g['name']
        if n:
            seen[n] = seen.get(n, 0) + 1
            if seen[n] > 1:
                g['name'] = f'{n} #{seen[n]}'
                n_renamed += 1
    stats['duplicate_names_resolved'] = n_renamed
    # Stable order: by distance, then seed.
    out.sort(key=lambda g: (g['dist_mpc'], g['seed']))
    return out


def luminosity_rank(g):
    """Sort key, most luminous first: UNGC K-band luminosity, else total K, else McConnachie M_V."""
    if g.get('logLK_ungc') is not None:
        return (0, -g['logLK_ungc'], g['seed'])
    k = g.get('kt', g.get('kmag_ungc'))
    if k is not None:
        return (1, k + 5.0 * math.log10(1e3 / g['dist_kpc']), g['seed'])   # roughly M_K
    return (2, g.get('MV_mcc', 0.0), g['seed'])


def move_companions(galaxies):
    """Puts the fainter member of a close pair with redshift-independent distances at the brighter's
    distance (see COMPANION_ARCMIN). Returns the names of the moved galaxies with their shift."""
    direct = [g for g in galaxies if g['dist_method'] == DIST_DIRECT and g.get('cz') is not None]
    index = SkyIndex(1.0)
    for g in direct:
        index.add(g['u'], g)
    dist0 = {id(g): g['dist_kpc'] for g in direct}
    moved = []
    for g in direct:
        primary = g
        for _sep, h in index.query(g['u'], COMPANION_ARCMIN / 60.0):
            if h is g or abs(h['cz'] - g['cz']) > COMPANION_DV_KMS:
                continue
            if abs(5.0 * math.log10(dist0[id(h)] / dist0[id(g)])) > COMPANION_DM_MAG:
                continue
            if luminosity_rank(h) < luminosity_rank(primary):
                primary = h
        if primary is not g:
            g['dist_kpc'] = dist0[id(primary)]
            label = g.get('ungc_name') or g.get('local_name') or g['seed_key']
            other = primary.get('ungc_name') or primary.get('local_name') or primary['seed_key']
            moved.append(f'{label} -> {other} ({(dist0[id(g)] - g["dist_kpc"]) / 1e3:+.2f} Mpc)')
    return sorted(moved)


def new_galaxy(ra, dec):
    return {'u': unit_vector(ra, dec), 'ra': ra, 'dec': dec, 'src': 0, 'ids': set(), 'own_ids': set(),
            'dist_kpc': None, 'dist_method': DIST_HUBBLE, 'barred': False, 'dsph': False, 'local_name': None,
            'seed_key': ''}


# ---------------------------------------------------------------------------------------------
# Writers
# ---------------------------------------------------------------------------------------------


def name_table(names):
    """Deduplicated names -> (index map, offsets+blob bytes)."""
    index, ordered = {}, []
    for n in names:
        if n and n not in index:
            index[n] = len(ordered)
            ordered.append(n)
    blob = bytearray()
    offsets = [0]
    for n in ordered:
        b = n.encode('utf-8')
        if b'\0' in b:
            raise ValueError('NUL in name')
        blob += b
        offsets.append(len(blob))
    table = struct.pack(f'<{len(offsets)}I', *offsets) + bytes(blob)
    return index, len(ordered), len(blob), table


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def write_atomic(path, data):
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, path)


def write_stars(path, stars, limit_mag, radius_pc, source_version):
    index, n_names, n_bytes, table = name_table(s['name'] for s in stars)
    recs = bytearray()
    for s in stars:
        recs += STAR_RECORD.pack(s['pos'][0], s['pos'][1], s['pos'][2], s['absmag'], s['teff'], s['radius'],
                                 index[s['name']] if s['name'] else -1, s['hip'], s['flags'], 0)
    payload = bytes(recs) + table
    header = STAR_HEADER.pack(b'BHSSTARS', FORMAT_VERSION, HEADER_BYTES, len(stars), STAR_RECORD.size, n_names,
                              n_bytes, zlib.crc32(payload) & 0xFFFFFFFF, 0, limit_mag, radius_pc, source_version,
                              0, 0, 0)
    write_atomic(path, header + payload)
    return n_names


def write_galaxies(path, galaxies):
    index, n_names, n_bytes, table = name_table(g['name'] for g in galaxies)
    recs = bytearray()
    for g in galaxies:
        recs += GALAXY_RECORD.pack(g['pos'][0], g['pos'][1], g['pos'][2], g['diam_kpc'], g['reff_kpc'], g['q'],
                                   g['pa'], g['incl'], g['MV'], g['logm'], g['cz'], g['t'],
                                   index[g['name']] if g['name'] else -1, g['seed'], g['flags'], g['morph'],
                                   g['dist_method'])
    payload = bytes(recs) + table
    header = GALAXY_HEADER.pack(b'BHSGALXS', FORMAT_VERSION, HEADER_BYTES, len(galaxies), GALAXY_RECORD.size,
                                n_names, n_bytes, zlib.crc32(payload) & 0xFFFFFFFF, H0, OMEGA_M, 0, 0, 0, 0, 0)
    write_atomic(path, header + payload)
    return n_names


# ---------------------------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------------------------


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--cache', default=DEFAULT_CACHE, help=f'download cache directory (default {DEFAULT_CACHE})')
    ap.add_argument('--out', default=DEFAULT_OUT, help='output directory (default <repo>/assets/real)')
    ap.add_argument('--offline', action='store_true', help='never download; use only the cache')
    ap.add_argument('--refresh', action='store_true', help='download every source again')
    ap.add_argument('--no-verify', action='store_true',
                    help='accept static files whose SHA-256 differs from the pinned value')
    args = ap.parse_args(argv)
    global VERIFY_PINNED
    VERIFY_PINNED = not args.no_verify

    os.makedirs(args.cache, exist_ok=True)
    os.makedirs(args.out, exist_ok=True)
    t_start = time.time()
    log(f'cache: {args.cache}')
    log(f'output: {args.out}')

    log('fetching sources')
    paths = {}
    hyg_key = 'hyg'
    try:
        try:
            paths['hyg'] = fetch(SOURCES['hyg'], args.cache, args.offline, args.refresh)
        except (RuntimeError, ValueError) as e:
            log(f'  HYG v4.4 unavailable ({e}); trying the v4.1 GitHub mirror')
            paths['hyg'] = fetch(SOURCES['hyg_fallback'], args.cache, args.offline, args.refresh)
            hyg_key = 'hyg_fallback'
        for key in ('ungc1', 'ungc2', 'ungc6', 'mcc1', 'mcc2', 'mcc3', 'mcc4', 'twomrs', 'xsc', 'rc3'):
            paths[key] = fetch(SOURCES[key], args.cache, args.offline, args.refresh)
    except (RuntimeError, ValueError) as e:
        log(f'error: {e}')
        return 1

    stats = {}
    log('stars')
    hyg_rows = read_hyg(paths['hyg'])
    stars = build_stars(hyg_rows, stats)
    dists = sorted(s['dist'] for s in stars if not s['sun'] and s['mag'] <= CATALOG_LIMIT_APP_MAG)
    radius_pc = math.ceil(percentile(dists, 0.99) / 100.0) * 100.0
    hyg_version = 44 if hyg_key == 'hyg' else 41
    n_star_names = write_stars(os.path.join(args.out, 'stars.bin'), stars, CATALOG_LIMIT_APP_MAG, radius_pc,
                               hyg_version)

    log('galaxies')
    ungc = load_ungc(paths['ungc1'], paths['ungc2'], paths['ungc6'])
    mcc = load_mcconnachie(paths['mcc1'], paths['mcc2'], paths['mcc3'], paths['mcc4'])
    twomrs = load_2mrs(paths['twomrs'])
    xsc = load_xsc(paths['xsc'])
    rc3 = load_rc3(paths['rc3'])
    galaxies = build_galaxies(ungc, mcc, twomrs, xsc, rc3, stats)
    n_gal_names = write_galaxies(os.path.join(args.out, 'galaxies.bin'), galaxies)

    # Manifest (deterministic: no dates).
    outputs = {}
    for fn in ('stars.bin', 'galaxies.bin'):
        p = os.path.join(args.out, fn)
        outputs[fn] = {'bytes': os.path.getsize(p), 'sha256': sha256_file(p)}
    used_sources = [hyg_key, 'ungc1', 'ungc2', 'ungc6', 'mcc1', 'mcc2', 'mcc3', 'mcc4', 'twomrs', 'xsc', 'rc3']
    manifest = {
        'format_version': FORMAT_VERSION,
        'generator': 'tools/fetch_real_catalogs.py',
        'inputs': {k: {'title': SOURCES[k].title, 'file': SOURCES[k].filename, 'urls': SOURCES[k].urls,
                       'sha256': content_sha256(SOURCES[k], paths['hyg' if k.startswith('hyg') else k])}
                   for k in used_sources},
        'stars': {'count': len(stars), 'names': n_star_names, 'hyg_version': f'v{hyg_version / 10:.1f}',
                  'catalog_limit_app_mag': CATALOG_LIMIT_APP_MAG, 'catalog_radius_pc': radius_pc,
                  'catalog_radius_rule': '99th percentile of the distances of stars with V <= limit, rounded up to 100 pc',
                  'distance_estimated': stats['stars_distance_estimated'],
                  'distance_estimate_soft_limited': stats['stars_distance_soft_limited'],
                  'distance_estimate_range_pc': list(EST_RANGE_PC),
                  'literature_overrides': stats['stars_overridden'],
                  'teff_from_spectral_type': stats['stars_teff_from_spectral_type'],
                  'teff_hot_star_correction': stats['stars_teff_hot_correction'],
                  'skipped_nonstellar': stats.get('skipped_nonstellar', 0)},
        'galaxies': {'count': len(galaxies), 'names': n_gal_names, 'H0': H0, 'omega_m': OMEGA_M,
                     'by_distance_method': {}, 'by_morphology': {},
                     'ungc_sizes_extinction_corrected_over_1.3x': stats['ungc_sizes_extinction_corrected_over_1.3x'],
                     'ungc_sizes_correction_limited': stats['ungc_sizes_correction_limited'],
                     'm31_subgroup_rescaled': stats['m31_subgroup_rescaled'],
                     'companions_moved': stats['companions_moved'],
                     'positions_corrected': stats['positions_corrected']},
        'calibration': {k: v for k, v in stats.items() if k.startswith('calib') or k.startswith('typical')},
        'matching': {k: stats[k] for k in ('ungc_merged_with_mcconnachie', '2mrs_merged_with_local', 'rc3_matches',
                                           'hubble_velocity_below_300', 'duplicate_names_resolved')},
        'clusters': stats['clusters'],
        'outputs': outputs,
    }
    dm_names = ['hubble_flow', 'redshift_independent', 'cluster_member', 'photometric_estimate', 'literature']
    mo_names = ['spiral', 'barred_spiral', 'lenticular', 'elliptical', 'irregular']
    for g in galaxies:
        k = dm_names[g['dist_method']]
        manifest['galaxies']['by_distance_method'][k] = manifest['galaxies']['by_distance_method'].get(k, 0) + 1
        k = mo_names[g['morph']]
        manifest['galaxies']['by_morphology'][k] = manifest['galaxies']['by_morphology'].get(k, 0) + 1
    manifest['galaxies']['type_guessed'] = sum(1 for g in galaxies if g['flags'] & G_TYPE_GUESS)
    manifest['galaxies']['pa_measured'] = sum(1 for g in galaxies if g['flags'] & G_PA)
    with open(os.path.join(args.out, 'manifest.json.tmp'), 'w', encoding='utf-8') as f:
        json.dump(manifest, f, indent=1, sort_keys=True, ensure_ascii=False)
        f.write('\n')
    os.replace(os.path.join(args.out, 'manifest.json.tmp'), os.path.join(args.out, 'manifest.json'))

    # Summary.
    log('')
    log(f'stars.bin     {outputs["stars.bin"]["bytes"]:>10,} bytes  {len(stars):,} stars, {n_star_names:,} names '
        f'(HYG v{hyg_version / 10:.1f}); {stats["stars_distance_estimated"]:,} distances estimated from spectral types')
    log(f'              completeness V <= {CATALOG_LIMIT_APP_MAG} within {radius_pc:.0f} pc of the Sun')
    log(f'              {stats["stars_distance_soft_limited"]:,} estimates outside {EST_RANGE_PC[0]:.0f}-{EST_RANGE_PC[1]:.0f} pc '
        f'(soft limits); {stats["stars_overridden"]} stars corrected from the literature')
    log(f'galaxies.bin  {outputs["galaxies.bin"]["bytes"]:>10,} bytes  {len(galaxies):,} galaxies, {n_gal_names:,} names')
    log(f'              distances: {manifest["galaxies"]["by_distance_method"]}')
    log(f'              morphology: {manifest["galaxies"]["by_morphology"]} '
        f'(guessed {manifest["galaxies"]["type_guessed"]:,}), PA measured {manifest["galaxies"]["pa_measured"]:,}')
    log(f'              calibration: {manifest["calibration"]}')
    log(f'              matching: {manifest["matching"]}')
    log(f'              UNGC sizes grown > 1.3x by the extinction correction: '
        f'{stats["ungc_sizes_extinction_corrected_over_1.3x"]} (limited beyond A_B = {UNGC_AB_TRUSTED}: '
        f'{stats["ungc_sizes_correction_limited"]}); M31 sub-group x{stats["m31_subgroup_rescaled"]["factor"]} '
        f'({stats["m31_subgroup_rescaled"]["members"]} galaxies)')
    log(f'              companions moved to their primary: {stats["companions_moved"]}')
    log(f'              positions corrected (2MASS and RC3 agree): {stats["positions_corrected"]}')
    log(f'              clusters: ' + ', '.join(f'{k} {v["members"]} at {v["distance_mpc"]} Mpc'
                                                for k, v in stats['clusters'].items()))
    for probe in ('Andromeda Galaxy (M31)', 'M32 (NGC 221)', 'M110 (NGC 205)', 'Triangulum Galaxy (M33)',
                  'Large Magellanic Cloud', 'Small Magellanic Cloud', "Bode's Galaxy (M81)", 'Cigar Galaxy (M82)',
                  'Virgo A (M87)', 'Centaurus A (NGC 5128)', 'Sombrero Galaxy (M104)', 'Whirlpool Galaxy (M51)',
                  'NGC 5195', 'Maffei 1', 'Maffei 2', 'Hidden Galaxy (IC 342)', 'Circinus Galaxy (ESO 97-G13)'):
        hits = [g for g in galaxies if g['name'] == probe]
        if hits:
            g = hits[0]
            log(f'              {probe:28s} {g["dist_mpc"] * 1e3:9.1f} kpc  D25 {g["diam_kpc"]:6.1f} kpc  '
                f'M_V {g["MV"]:6.2f}  logM* {g["logm"]:5.2f}  q {g["q"]:.2f} PA {g["pa"]:5.1f} i {g["incl"]:4.1f}')
        else:
            log(f'              {probe:28s} MISSING')
    log(f'manifest.json written; total {time.time() - t_start:.1f} s')
    return 0


if __name__ == '__main__':
    sys.exit(main())
