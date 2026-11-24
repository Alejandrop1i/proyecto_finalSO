/*
 * metrics.c - tablas hash de direccionamiento abierto y reporte de metricas.
 */
#include <stdlib.h>
#include <string.h>
#include "metrics.h"

#define HT_INITIAL_CAP 4096

/* ---------- tabla hash ---------- */

static unsigned long hash_str(const char *s)
{
    /* FNV-1a de 64 bits: rapido y suficiente para cadenas cortas. */
    unsigned long long h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return (unsigned long)h;
}

static int ht_init(hashtable_t *t, size_t cap)
{
    t->slots = calloc(cap, sizeof *t->slots);
    if (!t->slots)
        return -1;
    t->cap = cap;
    t->used = 0;
    return 0;
}

static void ht_free(hashtable_t *t)
{
    free(t->slots);
    t->slots = NULL;
    t->cap = t->used = 0;
}

static ht_entry_t *ht_find_slot(hashtable_t *t, const char *key)
{
    size_t i = hash_str(key) & (t->cap - 1);
    for (;;) {
        ht_entry_t *e = &t->slots[i];
        if (e->key[0] == '\0' || strcmp(e->key, key) == 0)
            return e;
        i = (i + 1) & (t->cap - 1);
    }
}

static int ht_grow(hashtable_t *t)
{
    hashtable_t n;
    if (ht_init(&n, t->cap * 2) < 0)
        return -1;
    for (size_t i = 0; i < t->cap; i++) {
        ht_entry_t *e = &t->slots[i];
        if (e->key[0]) {
            ht_entry_t *d = ht_find_slot(&n, e->key);
            *d = *e;
            n.used++;
        }
    }
    ht_free(t);
    *t = n;
    return 0;
}

/* Devuelve la entrada de key, creandola si no existe. NULL si no hay memoria. */
static ht_entry_t *ht_get(hashtable_t *t, const char *key)
{
    if (t->used * 10 >= t->cap * 7) {      /* factor de carga 70 % */
        if (ht_grow(t) < 0)
            return NULL;
    }
    ht_entry_t *e = ht_find_slot(t, key);
    if (e->key[0] == '\0') {
        strncpy(e->key, key, sizeof e->key - 1);
        e->key[sizeof e->key - 1] = '\0';
        t->used++;
    }
    return e;
}

/* ---------- metricas ---------- */

int metrics_init(metrics_t *m)
{
    memset(m, 0, sizeof *m);
    if (ht_init(&m->ips, HT_INITIAL_CAP) < 0)
        return -1;
    if (ht_init(&m->urls, HT_INITIAL_CAP) < 0) {
        ht_free(&m->ips);
        return -1;
    }
    return 0;
}

void metrics_free(metrics_t *m)
{
    ht_free(&m->ips);
    ht_free(&m->urls);
}

int metrics_add(metrics_t *m, const log_entry_t *e)
{
    ht_entry_t *h;

    m->total++;
    m->bytes += (unsigned long long)e->bytes;
    {
        int c = e->status / 100;
        m->status[(c >= 1 && c <= 5) ? c : 0]++;
    }

    h = ht_get(&m->ips, e->ip);
    if (!h)
        return -1;
    h->count++;

    h = ht_get(&m->urls, e->url);
    if (!h)
        return -1;
    h->count++;
    if (e->has_time) {
        h->time_sum += e->resp_time;
        h->time_count++;
        m->with_time++;
    }
    return 0;
}

void metrics_add_malformed(metrics_t *m, const char *line, size_t len)
{
    m->malformed++;
    if (m->n_bad_samples < LP_BAD_SAMPLES) {
        char *dst = m->bad_samples[m->n_bad_samples++];
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            len--;
        if (len >= LP_BAD_SAMPLE_LEN)
            len = LP_BAD_SAMPLE_LEN - 1;
        memcpy(dst, line, len);
        dst[len] = '\0';
    }
}

static int ht_merge(hashtable_t *dst, const hashtable_t *src)
{
    for (size_t i = 0; i < src->cap; i++) {
        const ht_entry_t *e = &src->slots[i];
        if (!e->key[0])
            continue;
        ht_entry_t *d = ht_get(dst, e->key);
        if (!d)
            return -1;
        d->count      += e->count;
        d->time_sum   += e->time_sum;
        d->time_count += e->time_count;
    }
    return 0;
}

int metrics_merge(metrics_t *dst, const metrics_t *src)
{
    dst->total     += src->total;
    dst->malformed += src->malformed;
    dst->bytes     += src->bytes;
    dst->with_time += src->with_time;
    for (int i = 0; i < 6; i++)
        dst->status[i] += src->status[i];
    for (int i = 0; i < src->n_bad_samples && dst->n_bad_samples < LP_BAD_SAMPLES; i++) {
        memcpy(dst->bad_samples[dst->n_bad_samples], src->bad_samples[i], LP_BAD_SAMPLE_LEN);
        dst->n_bad_samples++;
    }
    if (ht_merge(&dst->ips, &src->ips) < 0)
        return -1;
    if (ht_merge(&dst->urls, &src->urls) < 0)
        return -1;
    return 0;
}

/* ---------- top 10 ---------- */

typedef struct {
    const ht_entry_t *e;
    double            score;
} ranked_t;

/*
 * Selecciona los k mejores por score sin ordenar toda la tabla.
 * Devuelve cuantos encontro (puede ser menos de k).
 */
static int top_k(const hashtable_t *t, int k, ranked_t *out, int by_avg_time)
{
    int n = 0;
    for (size_t i = 0; i < t->cap; i++) {
        const ht_entry_t *e = &t->slots[i];
        if (!e->key[0])
            continue;
        double score;
        if (by_avg_time) {
            if (e->time_count == 0)
                continue;             /* sin tiempo no entra al ranking de lentas */
            score = e->time_sum / (double)e->time_count;
        } else {
            score = (double)e->count;
        }
        /* Insercion ordenada en un arreglo de maximo k elementos. */
        if (n < k || score > out[n - 1].score) {
            int pos = (n < k) ? n : k - 1;
            while (pos > 0 && out[pos - 1].score < score) {
                out[pos] = out[pos - 1];
                pos--;
            }
            out[pos].e = e;
            out[pos].score = score;
            if (n < k)
                n++;
        }
    }
    return n;
}

static void print_section(FILE *out, const char *title, const hashtable_t *t, int by_avg_time)
{
    ranked_t top[10];
    int n = top_k(t, 10, top, by_avg_time);
    fprintf(out, "\n%s\n", title);
    if (n == 0) {
        fprintf(out, "  (sin datos)\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        if (by_avg_time)
            fprintf(out, "  %2d. %-40s %8.4f s  (%lu muestras)\n", i + 1, top[i].e->key,
                    top[i].score, top[i].e->time_count);
        else
            fprintf(out, "  %2d. %-40s %10lu\n", i + 1, top[i].e->key, top[i].e->count);
    }
}

void metrics_print(const metrics_t *m, FILE *out, double elapsed)
{
    unsigned long lines = m->total + m->malformed;
    double rps = (elapsed > 0.0) ? (double)m->total / elapsed : 0.0;

    fprintf(out, "================ LogPulse: metricas ================\n");
    fprintf(out, "Lineas leidas:            %lu\n", lines);
    fprintf(out, "Peticiones validas:       %lu\n", m->total);
    fprintf(out, "Lineas mal formadas:      %lu\n", m->malformed);
    fprintf(out, "Tiempo de proceso:        %.3f s\n", elapsed);
    fprintf(out, "Peticiones por segundo:   %.0f\n", rps);
    fprintf(out, "Bytes transferidos:       %llu (%.2f MB)\n", m->bytes,
            (double)m->bytes / (1024.0 * 1024.0));
    fprintf(out, "\nCodigos HTTP\n");
    for (int c = 1; c <= 5; c++) {
        double pct = m->total ? 100.0 * (double)m->status[c] / (double)m->total : 0.0;
        fprintf(out, "  %dxx: %10lu  (%5.1f %%)\n", c, m->status[c], pct);
    }
    if (m->status[0])
        fprintf(out, "  otros: %8lu\n", m->status[0]);

    print_section(out, "Top 10 IPs con mas peticiones", &m->ips, 0);
    print_section(out, "Top 10 URLs mas solicitadas", &m->urls, 0);
    print_section(out, "Top 10 URLs con mayor tiempo de respuesta promedio", &m->urls, 1);

    if (m->n_bad_samples) {
        fprintf(out, "\nMuestra de lineas mal formadas (%d de %lu)\n", m->n_bad_samples, m->malformed);
        for (int i = 0; i < m->n_bad_samples; i++)
            fprintf(out, "  | %s\n", m->bad_samples[i]);
    }
    fprintf(out, "=====================================================\n");
}

int metrics_export_csv(const metrics_t *m, const char *path, double elapsed)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -1;
    fprintf(f, "seccion,clave,valor,extra\n");
    fprintf(f, "resumen,lineas_leidas,%lu,\n", m->total + m->malformed);
    fprintf(f, "resumen,peticiones_validas,%lu,\n", m->total);
    fprintf(f, "resumen,mal_formadas,%lu,\n", m->malformed);
    fprintf(f, "resumen,segundos,%.3f,\n", elapsed);
    fprintf(f, "resumen,peticiones_por_segundo,%.2f,\n",
            elapsed > 0 ? (double)m->total / elapsed : 0.0);
    fprintf(f, "resumen,bytes,%llu,\n", m->bytes);
    for (int c = 1; c <= 5; c++)
        fprintf(f, "codigos,%dxx,%lu,\n", c, m->status[c]);

    ranked_t top[10];
    int n;
    n = top_k(&m->ips, 10, top, 0);
    for (int i = 0; i < n; i++)
        fprintf(f, "top_ips,%s,%lu,\n", top[i].e->key, top[i].e->count);
    n = top_k(&m->urls, 10, top, 0);
    for (int i = 0; i < n; i++)
        fprintf(f, "top_urls,%s,%lu,\n", top[i].e->key, top[i].e->count);
    n = top_k(&m->urls, 10, top, 1);
    for (int i = 0; i < n; i++)
        fprintf(f, "urls_lentas,%s,%.4f,%lu\n", top[i].e->key, top[i].score, top[i].e->time_count);
    if (fclose(f) != 0)
        return -1;
    return 0;
}
