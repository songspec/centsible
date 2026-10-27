"""Beatswitch on the real songs in testdata/real: false alarms on untouched songs, real songs played through a known varispeed ramp, real songs spliced together.
Runs tests/beatswitch_eval (build it first) one batch after the other and writes the logs to results/beatswitch/final/.
   python tools/beatswitch_real_eval.py [path\\to\\beatswitch_eval.exe]            everything (about 25 minutes)
   python tools/beatswitch_real_eval.py exe --only varispeed|splice|untouched
"""
import io
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REAL = os.path.join(ROOT, 'testdata', 'real')
OUT = os.path.join(ROOT, 'results', 'beatswitch', 'final')
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

VARI_UP = ["02 - Mr. Brightside.flac", "03 - Caught In A Jam.flac", "04. Ken Carson - gynecologist.flac", "06 - Nobody's Son.flac", "09 All Star.flac", "12. FREAK NEEK.flac",
           "13. Tofu.flac", "15. Three worlds apart.flac", "10. Glass.flac", "3. Gracious.flac", "5. End of spring.flac", "12. FW19.flac"]
VARI_DOWN = ["0109 - Like you always do.flac", "1-32 - Attack of the Killer Queen.mp3", "06. x2.flac", "12 - One Dance.flac", "14 Lil Nas X - Panini.flac", "15. Che - EARDRUMMER.flac",
             "17. STAGEDIVIN.flac", "22 - Rubbin off the Paint.flac", "6. Noir Kei.flac", "7. Pray for.flac", "9. Like glue.flac", "Eminem - Relapse - 01 - Forever.flac"]
# (song B, tB, tA, [songs A])
SPLICES = [("0103 - The hero never dies.flac", 20, 70, ["02 - Mr. Brightside.flac", "10. Glass.flac", "15. Che - EARDRUMMER.flac", "6. Noir Kei.flac"]),
           ("5. End of spring.flac", 30, 70, ["0109 - Like you always do.flac", "03 - Caught In A Jam.flac", "0103 - The hero never dies.flac"]),
           ("13. Tofu.flac", 20, 60, ["04. Ken Carson - gynecologist.flac", "05. Ken Carson - wrist.flac"]),
           ("15. Three worlds apart.flac", 20, 70, ["10. Glass.flac", "06 - Nobody's Son.flac"]),
           ("7. Pray for.flac", 10, 60, ["6. Noir Kei.flac"])]


def run(exe, args, log):
    print('running', os.path.basename(log))
    with open(log, 'w', encoding='utf-8') as f:
        p = subprocess.run([exe] + args, cwd=REAL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        f.write(p.stdout.decode('utf-8', errors='replace'))
        f.write('exit %d\n' % p.returncode)


def summarise(logs):
    tot = passed = 0
    for log in logs:
        for line in open(log, encoding='utf-8', errors='replace'):
            if '||' not in line:
                continue
            tot += 1
            ok = line.split('||')[-1].strip().startswith('PASS')
            passed += ok
            print('%s %-34.34s %s' % ('ok  ' if ok else 'FAIL', line.split('|')[0].strip(), line.split('||')[-1].strip()[:200]))
    print('=> %d of %d pass' % (passed, tot))


def main():
    argv = sys.argv[1:]
    only = ''
    if '--only' in argv:
        i = argv.index('--only')
        only = argv[i + 1]
        del argv[i:i + 2]
    exe = argv[0] if argv else r'C:\cb\tests\Release\beatswitch_eval.exe'
    os.makedirs(OUT, exist_ok=True)
    curves = os.path.join(OUT, 'curves')
    os.makedirs(curves, exist_ok=True)
    if only in ('', 'varispeed'):
        run(exe, ['--seconds', '150', '--dump', curves, '--varispeed', '1.0', '1.08', '20', '80'] + VARI_UP, os.path.join(OUT, 'varispeed_up.log'))
        run(exe, ['--seconds', '150', '--dump', curves, '--varispeed', '1.0', '0.93', '20', '80'] + VARI_DOWN, os.path.join(OUT, 'varispeed_down.log'))
        print('\n== varispeed (real songs through a known ramp)')
        summarise([os.path.join(OUT, 'varispeed_up.log'), os.path.join(OUT, 'varispeed_down.log')])
    if only in ('', 'splice'):
        logs = []
        for i, (b, tb, ta, a_songs) in enumerate(SPLICES):
            log = os.path.join(OUT, 'splice_%d.log' % (i + 1))
            run(exe, ['--splice', b, str(ta), str(tb)] + a_songs, log)
            logs.append(log)
        print('\n== splice (song A up to tA, then song B from tB)')
        summarise(logs)
    if only in ('', 'untouched'):
        run(exe, ['--dump', curves, '--csv', os.path.join(OUT, 'untouched.txt'), '.'], os.path.join(OUT, 'untouched.log'))
        print('\n== untouched songs: see tools/beatswitch_compare.py results/beatswitch/final/untouched.log --list')


main()
