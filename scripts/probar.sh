#!/usr/bin/env bash
# Prueba de consistencia: loggen dice cuantas lineas escribio y cuantas estan
# mal formadas; logpulse debe contar exactamente lo mismo.
# Uso: bash scripts/probar.sh [lineas] [hilos]

set -u
N="${1:-200000}"
T="${2:-4}"
TMP="prueba_$$.log"

if [ ! -x ./logpulse ] || [ ! -x ./loggen ]; then
    echo "Compila primero con: make" >&2
    exit 1
fi

gen_out=$(./loggen -n "$N" -o "$TMP" -s 7 2>&1)
gen_total=$(echo "$gen_out" | sed -n 's/loggen: \([0-9]*\) lineas.*/\1/p')
gen_bad=$(echo "$gen_out" | sed -n 's/.*, \([0-9]*\) mal formadas.*/\1/p')

lp_out=$(./logpulse -m lote -q -t "$T" "$TMP")
lp_total=$(echo "$lp_out" | sed -n 's/^Lineas leidas: *\([0-9]*\).*/\1/p')
lp_bad=$(echo "$lp_out" | sed -n 's/^Lineas mal formadas: *\([0-9]*\).*/\1/p')

rm -f "$TMP"

echo "loggen  : $gen_total lineas, $gen_bad mal formadas"
echo "logpulse: $lp_total lineas, $lp_bad mal formadas"
if [ "$gen_total" = "$lp_total" ] && [ "$gen_bad" = "$lp_bad" ]; then
    echo "OK: los conteos coinciden"
    exit 0
else
    echo "FALLO: los conteos no coinciden" >&2
    exit 1
fi
