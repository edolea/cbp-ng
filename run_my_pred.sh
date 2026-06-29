#!/usr/bin/env zsh
# run_my_pred.sh — simple runner for my_pred<> values (three indices)
#
# Usage:
#   ./run_my_pred.sh        # runs ./compile for my_pred<n,m,k> over default ranges
#   ./run_my_pred.sh --dry-run

set -euo pipefail

DRY_RUN=false
for a in "$@"; do
  case "$a" in
    --dry-run|-n) DRY_RUN=true ;;
    *) echo "Unknown option: $a" >&2; exit 2 ;;
  esac
done

BHT_START=12
BHT_END=14
SHORT_H_START=12
SHORT_H_END=14
H_START=12
H_END=16

outdir="OUTPUT"
outfile="$outdir/all_outputs.txt"

echo "Running ./compile cbp -DPREDICTOR=\"my_pred<...,...,...>\" for n values $BHT_START..$BHT_END, m values $SHORT_H_START..$SHORT_H_END and k values $H_START..$H_END"
if $DRY_RUN; then
  echo "(dry run — commands will only be printed)"
  echo "All outputs would be appended to: $outfile"
else
  # ensure output directory exists and start with an empty combined output file
  if [[ ! -d "$outdir" ]]; then
    mkdir -p "$outdir"
  fi
  : > "$outfile"
fi

for ((n=BHT_START; n<=BHT_END; n++)); do
  for ((m=SHORT_H_START; m<=SHORT_H_END; m++)); do
    for ((k=H_START; k<=H_END; k++)); do
      cmd=("./compile" "cbp" "-DPREDICTOR=my_pred_tage<${n},${m},${k}>")
      echo "=== [${n},${m},${k}] ${cmd[*]}"
      if ! $DRY_RUN; then
        "${cmd[@]}"
        rc=$?
        if (( rc != 0 )); then
          echo "Command failed with status $rc; aborting." >&2
          exit $rc
        fi
      fi

      # After successful compile, run the binary on the test trace
      run_cmd=("./cbp" "./gcc_test_trace.gz" test 1000000 40000000)
      echo "    -> ${run_cmd[*]}  | sed 's/^/my_pred<${n},${m},${k}>: /' >> ${outfile}  (appending)"
      if ! $DRY_RUN; then
        # run and append both stdout and stderr, prefixing each line with identifier
        "${run_cmd[@]}" 2>&1 | sed "s/^/my_pred<${n},${m},${k}>: /" >> "$outfile"
        rc=$?
        if (( rc != 0 )); then
          echo "Run command failed with status $rc; aborting." >&2
          exit $rc
        fi
      fi
    done
  done
done

echo "All done."

