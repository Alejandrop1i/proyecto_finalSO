/*
 * bbuffer.h - bufer circular acotado (productor-consumidor).
 *
 * El hilo lector mete bloques de texto y los hilos parseadores los sacan.
 * Sincronizacion con un mutex y dos variables de condicion: si el bufer
 * esta lleno el lector se bloquea, si esta vacio los parseadores se bloquean.
 * Nunca hay espera activa.
 *
 * Un bloque con data == NULL es la "pildora venenosa": le dice al parseador
 * que ya no habra mas trabajo y que debe terminar.
 */
#ifndef LP_BBUFFER_H
#define LP_BBUFFER_H

#include <stddef.h>
#include <pthread.h>
#include "common.h"

typedef struct {
    char  *data;   /* memoria reservada por el productor, la libera el consumidor */
    size_t len;    /* bytes validos en data */
} chunk_t;

typedef struct {
    chunk_t         slots[LP_BUFFER_SLOTS];
    size_t          head;      /* siguiente posicion para sacar */
    size_t          tail;      /* siguiente posicion para meter */
    size_t          count;     /* bloques ocupados */
    pthread_mutex_t mtx;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
    /* Estadisticas para el informe: cuantas veces alguien tuvo que esperar. */
    unsigned long   waits_full;
    unsigned long   waits_empty;
} bbuffer_t;

int     bbuffer_init(bbuffer_t *b);
void    bbuffer_destroy(bbuffer_t *b);
/* Mete un bloque. Se bloquea si el bufer esta lleno. */
void    bbuffer_push(bbuffer_t *b, chunk_t c);
/* Saca un bloque. Se bloquea si el bufer esta vacio. */
chunk_t bbuffer_pop(bbuffer_t *b);
/* Cuantos bloques hay ahora (solo informativo). */
size_t  bbuffer_count(bbuffer_t *b);

#endif
