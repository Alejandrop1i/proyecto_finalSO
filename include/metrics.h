/*
 * metrics.h - acumulacion de metricas y tablas hash para IPs y URLs.
 *
 * Cada hilo parseador tiene su propio metrics_t y el publicador los fusiona
 * con metrics_merge. Asi no hay un contador global que bloquear por linea.
 */
#ifndef LP_METRICS_H
#define LP_METRICS_H

#include <stdio.h>
#include "parser.h"

/* Entrada de tabla hash. Sirve tanto para IPs como para URLs. */
typedef struct {
    char          key[LP_URL_LEN];
    unsigned long count;       /* peticiones con esta clave */
    double        time_sum;    /* suma de tiempos de respuesta (solo URLs) */
    unsigned long time_count;  /* cuantas lineas traian tiempo (solo URLs) */
} ht_entry_t;

typedef struct {
    ht_entry_t *slots;
    size_t      cap;   /* siempre potencia de dos */
    size_t      used;
} hashtable_t;

typedef struct {
    unsigned long      total;         /* lineas bien formadas */
    unsigned long      malformed;     /* lineas mal formadas */
    unsigned long      status[6];     /* indice 1..5 = 1xx..5xx, 0 = otros */
    unsigned long long bytes;
    unsigned long      with_time;     /* lineas que traian tiempo */
    hashtable_t        ips;
    hashtable_t        urls;
    char               bad_samples[LP_BAD_SAMPLES][LP_BAD_SAMPLE_LEN];
    int                n_bad_samples;
} metrics_t;

int  metrics_init(metrics_t *m);
void metrics_free(metrics_t *m);

/* Suma una linea bien formada. */
int  metrics_add(metrics_t *m, const log_entry_t *e);
/* Cuenta una linea mal formada y guarda una muestra si hay espacio. */
void metrics_add_malformed(metrics_t *m, const char *line, size_t len);
/* dst += src. src no se modifica. */
int  metrics_merge(metrics_t *dst, const metrics_t *src);

/* Imprime el reporte en texto plano. elapsed son los segundos de proceso. */
void metrics_print(const metrics_t *m, FILE *out, double elapsed);
/* Exporta las metricas a CSV. Devuelve 0 si pudo escribir. */
int  metrics_export_csv(const metrics_t *m, const char *path, double elapsed);

#endif
