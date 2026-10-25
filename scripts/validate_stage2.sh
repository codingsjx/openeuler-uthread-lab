#!/bin/sh
# All work runs as the logged-in user. No sudo or system configuration changes.
set -u
cd "$(dirname "$0")/.." || exit 1
mkdir -p results
printf '{"status":"running"}\n' > results/stage2-status.json
validate() {
    make check &&
    python3 -m unittest discover -s tests -p test_reporting.py -v &&
    python3 scripts/benchmark.py &&
    python3 scripts/make_report.py
}
validate > results/stage2-validation.log 2>&1
outcome=$?
if [ "$outcome" -eq 0 ]; then
    printf '{"status":"passed","exit_code":0}\n' > results/stage2-status.json
else
    printf '{"status":"failed","exit_code":%s}\n' "$outcome" > results/stage2-status.json
fi
python3 scripts/bundle_results.py
printf 'STAGE2_EXIT=%s\n' "$outcome"
exit "$outcome"
