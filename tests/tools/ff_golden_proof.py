#!/usr/bin/env python3
"""Attribute every difference between two fixed-function golden dumps to a
named fixed site.

A deliberate change to the shared fixed-function generator re-records
tests/fixtures/ff_d3d_golden.txt. This proves that the re-record touched only
what the change meant to: every changed MSL line must match one of SITES, and
every changed vertex-uniform member must match UNIFORM_SITES. Anything else
is printed as UNCLASSIFIED and the exit status is 1.

Make the dumps with the golden test (tests/graphics/test_ff_emulation_d3d_golden.cpp):
  1. check out the base commit's src/graphics/shaders/ff_* over the tree
     (keep the current test), build vitreo_graphics_tests and run
       VITREO_FF_GOLDEN_DUMP=base.txt build/tests/vitreo_graphics_tests \\
           '--gtest_filter=FFEmulationD3DGolden.*'
  2. restore the change, rebuild, dump to new.txt the same way;
  3. python3 tests/tools/ff_golden_proof.py base.txt new.txt

SITES and UNIFORM_SITES describe the Task 8b matrix-convention fix and its
lighting follow-up; edit them for a different change.
"""
import difflib
import re
import sys
from collections import Counter

SITES = [
    ('position', r'out\.position = '),
    ('fog/point-scale camera space', r'(point_eye|eye_position) = '),
    ('TCI camera space', r'tci_(world_pos|view_pos|world_n|view_n) = |float4 object_normal = '),
    ('lighting camera-space position', r'float3 world_pos = '),
    ('lighting camera-space normal', r'float3 world_normal = |float4 lighting_object_normal = '),
    ('texture matrix', r'tc = (tc \* )?cb\.texture_matrix'),
    ('PROJECTED divisor', r'float2 uv = tc\.xy / '),
    ('light Range cutoff', r'if \(dist > cb\.light\d+_direction\.w\) atten = 0\.0;'),
    ('spot factor (Theta/Phi/Falloff)',
     r'spot_cos|spot_atten|float rho = |spot_k|spot_factor|: \(rho <= |: pow\(\(rho|float4 light\d+_spot;'),
]
UNIFORM_SITES = [
    ('normal matrix = (W V)^-1', r'world\d*_inv_transpose|blend_world_inv_transposes'),
    ('camera at the camera-space origin', r'camera_position'),
    ('non-local viewer (0,0,1)', r'camera_direction'),
    ('D3D light in camera space', r'light\d+_(position|direction)'),
    ('spot cone cosines and falloff', r'light\d+_spot'),
]
SITES = [(n, re.compile(rx)) for n, rx in SITES]
UNIFORM_SITES = [(n, re.compile('^(' + rx + ')$')) for n, rx in UNIFORM_SITES]


def load(path):
    entries, cur = [], None
    with open(path) as f:
        for line in f:
            tag, _, rest = line.rstrip('\n').partition('|')
            if tag == 'E':
                cur = {'head': rest}
                entries.append(cur)
            else:
                cur[tag] = bytes.fromhex(rest)
    return entries


def uniform_members(msl, data):
    """{member name: its bytes} from the MSL FFUniforms struct layout."""
    m = re.search(r'struct FFUniforms \{(.*?)\n\};', msl, re.S)
    assert m, 'no FFUniforms struct'
    out, off = {}, 0
    for decl in m.group(1).split(';'):
        decl = decl.strip()
        if not decl:
            continue
        mm = re.match(r'(float4x4|float4|uint4|int4|float|uint)\s+(\w+)(?:\[(\d+)\])?$', decl)
        assert mm, decl
        size, align = {'float4x4': (64, 16), 'float4': (16, 16), 'uint4': (16, 16), 'int4': (16, 16),
                       'float': (4, 4), 'uint': (4, 4)}[mm.group(1)]
        off = (off + align - 1) // align * align
        n = size * int(mm.group(3) or 1)
        out[mm.group(2)] = data[off:off + n]
        off += n
    # The packer can append bytes the struct does not declare (for example
    # point-scale parameters for a pretransformed state); they are compared
    # as one more member.
    out['<bytes beyond the struct>'] = data[off:]
    return out


def changed_lines(a, b):
    out = []
    for tag, i1, i2, j1, j2 in difflib.SequenceMatcher(None, a, b, autojunk=False).get_opcodes():
        if tag != 'equal':
            out += [('-', l) for l in a[i1:i2]] + [('+', l) for l in b[j1:j2]]
    return out


def classify(name, table):
    for site, rx in table:
        if rx.search(name):
            return site
    return None


def main(base_path, new_path):
    base, new = load(base_path), load(new_path)
    assert len(base) == len(new), (len(base), len(new))
    unclassified, by_site, by_kind, unchanged = [], Counter(), Counter(), Counter()
    uniform_only = Counter()
    for o, n in zip(base, new):
        assert o['head'] == n['head'], (o['head'], n['head'])
        kind = o['head'].split('|')[2]
        sites, msl_changed = set(), False
        for tag in ('VM', 'FM'):
            if o[tag] == n[tag]:
                continue
            msl_changed = True
            for sign, line in changed_lines(o[tag].decode().split('\n'), n[tag].decode().split('\n')):
                s = line.strip()
                site = classify(s, SITES)
                if site:
                    sites.add(site)
                elif s.startswith('//') or s == '':
                    sites.add('comment / blank line')
                else:
                    unclassified.append((o['head'], tag, sign + ' ' + s))
        if o['VU'] != n['VU']:
            a = uniform_members(o['VM'].decode(), o['VU'])
            b = uniform_members(n['VM'].decode(), n['VU'])
            for member in sorted(set(a) | set(b)):
                if a.get(member) == b.get(member):
                    continue
                site = classify(member, UNIFORM_SITES)
                if site:
                    sites.add('uniform: ' + site)
                else:
                    unclassified.append((o['head'], 'VU', member))
        if o['FU'] != n['FU']:
            unclassified.append((o['head'], 'FU', 'fragment uniforms changed'))
        if sites or o['FU'] != n['FU']:
            by_kind[kind] += 1
            for s in sites:
                by_site[s] += 1
            if not msl_changed:
                uniform_only[kind] += 1
        else:
            unchanged[kind] += 1
    print('entries:', len(base))
    print('changed:', sum(by_kind.values()), dict(by_kind))
    print('  of which vertex uniforms only (no MSL change):', dict(uniform_only))
    print('unchanged:', sum(unchanged.values()), dict(unchanged))
    print('entries per site (an entry can reach several):')
    for s, c in by_site.most_common():
        print(f'  {c:5d}  {s}')
    print('UNCLASSIFIED differences:', len(unclassified))
    for u in unclassified[:40]:
        print('  ', u)
    return 1 if unclassified else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1], sys.argv[2]))
