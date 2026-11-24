/*
 * worker.h - un trabajador: hilo lector + N parseadores + hilo publicador.
 *
 * Fase 1: el trabajador corre dentro del proceso principal.
 * Fase 2 (integrante de procesos): el coordinador hace fork() y cada hijo
 * llama worker_run() sobre su archivo; el publicador escribe en la memoria
 * compartida en vez de stderr.
 * Fase 2 (integrante de archivos): el lector cambia read() por mmap() y
 * en modo vivo usa inotify.
 */
#ifndef LP_WORKER_H
#define LP_WORKER_H

#include "metrics.h"
#include "bbuffer.h"

typedef struct {
    const char *path;        /* archivo de log a procesar */
    int         nthreads;    /* hilos parseadores */
    int         quiet;       /* 1 = sin linea de progreso en stderr */
} worker_config_t;

typedef struct {
    metrics_t     metrics;      /* resultado final fusionado */
    double        elapsed;      /* segundos de proceso */
    unsigned long long bytes_read;
    unsigned long chunks;       /* bloques que pasaron por el bufer */
    unsigned long waits_full;   /* veces que el lector espero por bufer lleno */
    unsigned long waits_empty;  /* veces que un parseador espero por bufer vacio */
} worker_result_t;

/* Procesa el archivo completo. Devuelve 0 si todo salio bien. */
int worker_run(const worker_config_t *cfg, worker_result_t *res);

#endif
