#!/bin/bash

TOTAL_PROCESSES=3

echo "Lançando $TOTAL_PROCESSES processos MPI em terminais xterm separados..."

mpirun -np $TOTAL_PROCESSES \
    --bind-to none \
    --mca orte_base_help_aggregate 0 \
    --host localhost \
    -x DISPLAY \
    --map-by node \
    --rank-by node \
    --report-bindings \
    --display-map \
    --allow-run-as-root \
    --oversubscribe \
    bash ./launch_xterm_process.sh ./editor_colaborativo
