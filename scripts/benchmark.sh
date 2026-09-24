#!/bin/sh
set -eu
build="${BUILD_DIR:-build}"
records="${BENCH_RECORDS:-10000}"
out="${BENCH_OUT:-benchmark-results}"
mkdir -p "$out"
csv="$out/results.csv"
json="$out/results.jsonl"
echo "threads,message_bytes,attempted,producer_seconds,end_to_end_seconds,producer_logs_per_sec,end_to_end_logs_per_sec,enqueued,dropped,sync_fallbacks,queue_high_watermark,consumer_records,consumer_batches,queue_slot_bytes,queue_spill_capacity,queue_spill_exhaustions,queue_storage_bytes" > "$csv"
: > "$json"
for t in 1 2 4 8 16 32 64; do
  for size in 64 256 1024 4000; do
    line="$("$build/bench_matrix" "$t" "$records" "$size")"
    echo "$line" >> "$json"
    python3 - "$line" >> "$csv" <<'PY'
import json,sys
x=json.loads(sys.argv[1])
keys=["threads","message_bytes","attempted","producer_seconds","end_to_end_seconds","producer_logs_per_sec","end_to_end_logs_per_sec","enqueued","dropped","sync_fallbacks","queue_high_watermark","consumer_records","consumer_batches","queue_slot_bytes","queue_spill_capacity","queue_spill_exhaustions","queue_storage_bytes"]
print(",".join(str(x[k]) for k in keys))
PY
    printf '.'
  done
done
echo
echo "wrote $csv and $json"
