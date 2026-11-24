/*
 * worker.c - trabajador de un archivo: hilo lector, N parseadores, publicador.
 *
 * Flujo:
 *   lector  --bloques de 64 KB-->  bufer acotado  -->  parseadores (N)
 *   parseadores acumulan en metricas locales (una por hilo).
 *   publicador: cada segundo fusiona las locales y reporta progreso.
 *   al final: join de todos y fusion final.
 *
 * Terminacion ordenada: cuando el lector acaba (fin de archivo o g_stop),
 * mete N pildoras venenosas (data == NULL). Cada parseador sale al recibir
 * una. El publicador sale cuando main le avisa por su variable de condicion.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "worker.h"

/* Estado compartido entre los hilos de un trabajador. */
typedef struct {
    const worker_config_t *cfg;
    bbuffer_t   buffer;

    /* una metrica local por parseador, protegida por su propio mutex */
    metrics_t       *local;
    pthread_mutex_t *local_mtx;

    /* avisos entre main y el publicador */
    pthread_mutex_t  pub_mtx;
    pthread_cond_t   pub_cv;
    int              parsers_done;

    /* estadisticas del lector */
    unsigned long long bytes_read;
    unsigned long      chunks;
    int                reader_error;
    double             t_start;
} worker_state_t;

typedef struct {
    worker_state_t *st;
    int             idx;
} parser_arg_t;

/* ---------- hilo lector ---------- */

/*
 * Fase 1: lectura con read() por bloques.
 * TODO integrante de archivos: version con mmap() + madvise() para modo lote
 * y con inotify para modo vivo. La interfaz con el bufer no cambia: se siguen
 * metiendo chunk_t con lineas completas.
 */
static void *reader_thread(void *arg)
{
    worker_state_t *st = arg;
    int fd = open(st->cfg->path, O_RDONLY);
    if (fd < 0) {
        st->reader_error = errno;
        goto pills;
    }

    char   *carry = malloc(LP_CHUNK_SIZE);   /* trozo de linea incompleta */
    size_t  carry_len = 0;
    if (!carry) {
        st->reader_error = ENOMEM;
        close(fd);
        goto pills;
    }

    while (!g_stop) {
        /* El bloque lleva primero lo que quedo pendiente y luego lo nuevo. */
        char *buf = malloc(carry_len + LP_CHUNK_SIZE);
        if (!buf) {
            st->reader_error = ENOMEM;
            break;
        }
        memcpy(buf, carry, carry_len);
        ssize_t n = read(fd, buf + carry_len, LP_CHUNK_SIZE);
        if (n < 0) {
            if (errno == EINTR) {
                free(buf);
                continue;
            }
            st->reader_error = errno;
            free(buf);
            break;
        }
        if (n == 0) {
            /* Fin de archivo: si quedo una ultima linea sin salto, se manda. */
            if (carry_len > 0) {
                chunk_t c = { buf, carry_len };
                st->chunks++;
                bbuffer_push(&st->buffer, c);
            } else {
                free(buf);
            }
            break;
        }
        st->bytes_read += (unsigned long long)n;
        size_t total = carry_len + (size_t)n;

        /* Buscar el ultimo salto de linea para no partir una linea en dos. */
        size_t cut = total;
        while (cut > 0 && buf[cut - 1] != '\n')
            cut--;

        if (cut == 0) {
            /* Ni un salto de linea en todo el bloque: linea gigantesca.
             * Se guarda como pendiente hasta donde quepa. */
            if (total > LP_CHUNK_SIZE)
                total = LP_CHUNK_SIZE;   /* se descarta el exceso */
            memcpy(carry, buf, total);
            carry_len = total;
            free(buf);
            continue;
        }

        carry_len = total - cut;
        memcpy(carry, buf + cut, carry_len);

        chunk_t c = { buf, cut };
        st->chunks++;
        bbuffer_push(&st->buffer, c);
    }
    free(carry);
    close(fd);

pills:
    for (int i = 0; i < st->cfg->nthreads; i++) {
        chunk_t pill = { NULL, 0 };
        bbuffer_push(&st->buffer, pill);
    }
    return NULL;
}

/* ---------- hilos parseadores ---------- */

static void *parser_thread(void *arg)
{
    parser_arg_t   *pa = arg;
    worker_state_t *st = pa->st;
    metrics_t      *m  = &st->local[pa->idx];
    pthread_mutex_t *mtx = &st->local_mtx[pa->idx];
    log_entry_t     e;

    for (;;) {
        chunk_t c = bbuffer_pop(&st->buffer);
        if (c.data == NULL)
            break;                       /* pildora venenosa */

        /* Se bloquea una vez por bloque y no por linea: el publicador
         * espera como mucho lo que tarda en procesarse un bloque. */
        pthread_mutex_lock(mtx);
        const char *p   = c.data;
        const char *end = c.data + c.len;
        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t len = nl ? (size_t)(nl - p) + 1 : (size_t)(end - p);
            if (parse_line(p, len, &e) == 0)
                metrics_add(m, &e);
            else
                metrics_add_malformed(m, p, len);
            p += len;
        }
        pthread_mutex_unlock(mtx);
        free(c.data);
    }
    return NULL;
}

/* ---------- hilo publicador ---------- */

/* Fusiona todas las metricas locales en out (out debe venir inicializado). */
static int snapshot(worker_state_t *st, metrics_t *out)
{
    for (int i = 0; i < st->cfg->nthreads; i++) {
        pthread_mutex_lock(&st->local_mtx[i]);
        int rc = metrics_merge(out, &st->local[i]);
        pthread_mutex_unlock(&st->local_mtx[i]);
        if (rc < 0)
            return -1;
    }
    return 0;
}

/*
 * Fase 1: imprime una linea de progreso por segundo en stderr.
 * TODO integrante de procesos: en vez de imprimir, copiar el snapshot al
 * segmento de memoria compartida protegido por el semaforo, y mandar el
 * mensaje de estado por la tuberia.
 */
static void *publisher_thread(void *arg)
{
    worker_state_t *st = arg;

    pthread_mutex_lock(&st->pub_mtx);
    while (!st->parsers_done) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 1;
        /* Espera con variable de condicion y plazo: no es espera activa y
         * main puede despertarlo de inmediato cuando todo termino. */
        int rc = pthread_cond_timedwait(&st->pub_cv, &st->pub_mtx, &deadline);
        if (rc == ETIMEDOUT && !st->parsers_done && !st->cfg->quiet) {
            pthread_mutex_unlock(&st->pub_mtx);
            metrics_t snap;
            if (metrics_init(&snap) == 0) {
                snapshot(st, &snap);
                double el = lp_now() - st->t_start;
                fprintf(stderr, "[progreso %5.1f s] %lu lineas, %.0f lineas/s, bufer %zu/%d\n",
                        el, snap.total + snap.malformed,
                        el > 0 ? (double)(snap.total + snap.malformed) / el : 0.0,
                        bbuffer_count(&st->buffer), LP_BUFFER_SLOTS);
                metrics_free(&snap);
            }
            pthread_mutex_lock(&st->pub_mtx);
        }
    }
    pthread_mutex_unlock(&st->pub_mtx);
    return NULL;
}

/* ---------- orquestacion ---------- */

int worker_run(const worker_config_t *cfg, worker_result_t *res)
{
    worker_state_t st;
    int            n = cfg->nthreads;
    int            rc = -1;

    memset(&st, 0, sizeof st);
    memset(res, 0, sizeof *res);
    st.cfg = cfg;

    if (n < LP_MIN_THREADS || n > LP_MAX_THREADS) {
        fprintf(stderr, "logpulse: numero de hilos invalido (%d)\n", n);
        return -1;
    }
    if (bbuffer_init(&st.buffer) < 0)
        return -1;
    pthread_mutex_init(&st.pub_mtx, NULL);
    pthread_cond_init(&st.pub_cv, NULL);

    st.local     = calloc((size_t)n, sizeof *st.local);
    st.local_mtx = calloc((size_t)n, sizeof *st.local_mtx);
    pthread_t    *parsers = calloc((size_t)n, sizeof *parsers);
    parser_arg_t *pargs   = calloc((size_t)n, sizeof *pargs);
    if (!st.local || !st.local_mtx || !parsers || !pargs)
        goto cleanup;
    for (int i = 0; i < n; i++) {
        if (metrics_init(&st.local[i]) < 0)
            goto cleanup;
        pthread_mutex_init(&st.local_mtx[i], NULL);
    }
    if (metrics_init(&res->metrics) < 0)
        goto cleanup;

    st.t_start = lp_now();

    pthread_t reader, publisher;
    if (pthread_create(&reader, NULL, reader_thread, &st) != 0)
        goto cleanup;
    for (int i = 0; i < n; i++) {
        pargs[i].st  = &st;
        pargs[i].idx = i;
        if (pthread_create(&parsers[i], NULL, parser_thread, &pargs[i]) != 0) {
            /* Si falla uno, se cancela todo de forma ordenada: g_stop hace que
             * el lector mande las pildoras y los ya creados salgan. */
            g_stop = 1;
            for (int j = 0; j < i; j++)
                pthread_join(parsers[j], NULL);
            pthread_join(reader, NULL);
            goto cleanup;
        }
    }
    if (pthread_create(&publisher, NULL, publisher_thread, &st) != 0) {
        g_stop = 1;
        for (int i = 0; i < n; i++)
            pthread_join(parsers[i], NULL);
        pthread_join(reader, NULL);
        goto cleanup;
    }

    /* Terminacion ordenada: lector, parseadores, luego el publicador. */
    pthread_join(reader, NULL);
    for (int i = 0; i < n; i++)
        pthread_join(parsers[i], NULL);

    pthread_mutex_lock(&st.pub_mtx);
    st.parsers_done = 1;
    pthread_cond_signal(&st.pub_cv);
    pthread_mutex_unlock(&st.pub_mtx);
    pthread_join(publisher, NULL);

    res->elapsed = lp_now() - st.t_start;
    if (snapshot(&st, &res->metrics) < 0)
        goto cleanup;
    res->bytes_read  = st.bytes_read;
    res->chunks      = st.chunks;
    res->waits_full  = st.buffer.waits_full;
    res->waits_empty = st.buffer.waits_empty;

    if (st.reader_error) {
        fprintf(stderr, "logpulse: error leyendo %s: %s\n", cfg->path, strerror(st.reader_error));
        rc = -1;
    } else {
        rc = 0;
    }

cleanup:
    if (st.local) {
        for (int i = 0; i < n; i++) {
            metrics_free(&st.local[i]);
            pthread_mutex_destroy(&st.local_mtx[i]);
        }
    }
    free(st.local);
    free(st.local_mtx);
    free(parsers);
    free(pargs);
    pthread_mutex_destroy(&st.pub_mtx);
    pthread_cond_destroy(&st.pub_cv);
    /* Si algo quedo en el bufer (salida por error), se libera. */
    while (st.buffer.count > 0) {
        chunk_t c = bbuffer_pop(&st.buffer);
        free(c.data);
    }
    bbuffer_destroy(&st.buffer);
    return rc;
}
