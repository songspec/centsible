#!/bin/bash
# Runs the real-audio shift suite over the whole catalog twice: the shipped settings, then the v1.2.0-first-build settings (roll-off set of five, no disagreement flag).
cd /c/dev/detunetest
mkdir -p results/real_new results/real_old
/c/cb/tests/Release/centsible_real_tests.exe testdata/real results/real_new 180 > results/real_new/run.log 2>&1
echo "new done" >> results/real_new/run.log
CENTSIBLE_ALPHAS="0.6,1.2,2,3,4.5" CENTSIBLE_ARBITRATE=0 /c/cb/tests/Release/centsible_real_tests.exe testdata/real results/real_old 180 > results/real_old/run.log 2>&1
echo "old done" >> results/real_old/run.log
