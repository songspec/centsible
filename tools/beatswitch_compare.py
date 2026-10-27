"""Summarise one or more beatswitch_eval logs (verdict counts, boundaries per song) and list the songs that are not 'constant'.
   python tools/beatswitch_compare.py results/beatswitch/untouched_v1.log results/beatswitch/untouched3.log [--list]"""
import io
import re
import sys
from collections import Counter

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')


def load(path):
    rows = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        if ' | ' not in line:
            continue
        p = [x.strip() for x in line.rstrip('\n').split('|')]
        if len(p) < 7:
            continue
        m = re.search(r'(\d+) boundaries', p[6])
        rows[p[0]] = dict(verdict=p[2], n=int(m.group(1)) if m else 0, text=p[6], bpm=p[4], detune=p[5])
    return rows


def main():
    paths = [a for a in sys.argv[1:] if not a.startswith('--')]
    show = '--list' in sys.argv
    logs = [(p, load(p)) for p in paths]
    for p, r in logs:
        v = Counter(x['verdict'] for x in r.values())
        nb = sorted(x['n'] for x in r.values())
        print('%-40s %3d songs  %s' % (p.split('/')[-1].split('\\')[-1], len(r), dict(v)))
        print('%-40s boundaries: mean %.1f, median %d, none %d, at most one %d' % ('', sum(nb) / len(nb), nb[len(nb) // 2], sum(1 for n in nb if n == 0), sum(1 for n in nb if n <= 1)))
    if show and logs:
        last = logs[-1][1]
        for k, x in sorted(last.items(), key=lambda kv: (kv[1]['verdict'], -kv[1]['n'])):
            if x['verdict'] != 'constant' or x['n'] > 0:
                print('%-14s %-40.40s %2d  %s' % (x['verdict'], k, x['n'], x['text'][:140]))


main()
