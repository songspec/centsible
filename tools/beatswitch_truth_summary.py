"""Summarise beatswitch_eval --varispeed / --splice logs: PASS / FAIL per song and the totals.
   python tools/beatswitch_truth_summary.py results/beatswitch/vari2_A.log results/beatswitch/vari2_B.log"""
import io
import re
import sys

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
tot = passed = verd = refok = 0
for f in sys.argv[1:]:
    print('==', f)
    for l in open(f, encoding='utf-8', errors='replace'):
        if '||' not in l:
            continue
        name = l.split('|')[0].strip()
        tail = l.split('||')[-1].strip()
        tot += 1
        passed += tail.startswith('PASS')
        verd += 'verdict varispeed' in tail
        m = re.search(r'off ([+-]?\d+) cents', tail)
        refok += bool(m) and abs(int(m.group(1))) <= 20
        print('%-36.36s %s' % (name, tail[:190]))
print('songs %d, PASS %d, verdict varispeed %d, reference tempo within 20 cents %d' % (tot, passed, verd, refok))
