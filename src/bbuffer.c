/*
 * bbuffer.c - bufer circular acotado con mutex y variables de condicion.
 */
#include <string.h>
#include "bbuffer.h"

int bbuffer_init(bbuffer_t *b)
{
    memset(b, 0, sizeof *b);
    if (pthread_mutex_init(&b->mtx, NULL) != 0)
        return -1;
    if (pthread_cond_init(&b->not_full, NULL) != 0)
        return -1;
    if (pthread_cond_init(&b->not_empty, NULL) != 0)
        return -1;
    return 0;
}

void bbuffer_destroy(bbuffer_t *b)
{
    pthread_mutex_destroy(&b->mtx);
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
}

void bbuffer_push(bbuffer_t *b, chunk_t c)
{
    pthread_mutex_lock(&b->mtx);
    /* Siempre while y no if: un despertar espurio o una carrera entre varios
     * hilos podria dejar el bufer lleno otra vez antes de que nos toque. */
    while (b->count == LP_BUFFER_SLOTS) {
        b->waits_full++;
        pthread_cond_wait(&b->not_full, &b->mtx);
    }
    b->slots[b->tail] = c;
    b->tail = (b->tail + 1) % LP_BUFFER_SLOTS;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mtx);
}

chunk_t bbuffer_pop(bbuffer_t *b)
{
    chunk_t c;
    pthread_mutex_lock(&b->mtx);
    while (b->count == 0) {
        b->waits_empty++;
        pthread_cond_wait(&b->not_empty, &b->mtx);
    }
    c = b->slots[b->head];
    b->head = (b->head + 1) % LP_BUFFER_SLOTS;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mtx);
    return c;
}

size_t bbuffer_count(bbuffer_t *b)
{
    size_t n;
    pthread_mutex_lock(&b->mtx);
    n = b->count;
    pthread_mutex_unlock(&b->mtx);
    return n;
}
