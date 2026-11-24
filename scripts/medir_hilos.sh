#!/usr/bin/env bash
# Mide lineas por segundo con 1, 2, 4 y 8 hilos parseadores.
# Uso: bash scripts/medir_hilos.sh [archivo.log] [repeticiones]
# Resultado: resultados/hilos.csv (columnas: hilos,repeticion,lineas,segundos,lineas_por_segundo)

set -u
LOG="${1:-access.log}"
REPS="${2:-3}"
OUT_DIR="resultados"
OUT="$OUT_DIR/hilos.csv"

if [ ! -f "$LOG" ]; then
    echo "No existe $LOG. Genera uno con: make gen" >&2
    exit 1
fi
if [ ! -x ./logpulse ]; then
    echo "Falta ./logpulse. Compila con: make" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
echo "hilos,repeticion,lineas,segundos,lineas_por_segundo" > "$OUT"
echo "Maquina: $(nproc) nucleos, $(uname -sr)"
echo "Archivo: $LOG ($(wc -l < "$LOG") lineas)"

for t in 1 2 4 8; do
    for r in $(seq 1 "$REPS"); do
        line=$(./logpulse -m lote -q -t "$t" "$LOG" | grep '^RESUMEN')
        lineas=$(echo "$line" | sed -n 's/.*lineas=\([0-9]*\).*/\1/p')
        seg=$(echo "$line" | sed -n 's/.*segundos=\([0-9.]*\).*/\1/p')
        lps=$(echo "$line" | sed -n 's/.*lineas_por_segundo=\([0-9]*\).*/\1/p')
        echo "$t,$r,$lineas,$seg,$lps" >> "$OUT"
        printf "hilos=%d rep=%d  %s lineas en %s s  -> %s lineas/s\n" "$t" "$r" "$lineas" "$seg" "$lps"
    done
done
echo "Guardado en $OUT"
