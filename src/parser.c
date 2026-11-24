/*
 * parser.c - parseo manual de una linea en formato combinado.
 *
 * Se recorre la linea con punteros en vez de sscanf porque es mas rapido
 * y porque asi controlamos exactamente que se acepta y que no.
 */
#include <string.h>
#include <stdlib.h>
#include "parser.h"

/* Copia [a, b) en dst con tope. Devuelve -1 si no cabe o esta vacio. */
static int copy_field(char *dst, size_t cap, const char *a, const char *b)
{
    size_t n = (size_t)(b - a);
    if (n == 0 || n >= cap)
        return -1;
    memcpy(dst, a, n);
    dst[n] = '\0';
    return 0;
}

/* Avanza p hasta el siguiente espacio o fin. */
static const char *skip_token(const char *p, const char *end)
{
    while (p < end && *p != ' ')
        p++;
    return p;
}

/* Devuelve la comilla de cierre de una cadena que empieza en p (p apunta a la comilla de apertura). */
static const char *quoted_end(const char *p, const char *end)
{
    if (p >= end || *p != '"')
        return NULL;
    p++;
    while (p < end && *p != '"') {
        if (*p == '\\' && p + 1 < end)
            p++;
        p++;
    }
    return (p < end) ? p : NULL;
}

int parse_line(const char *line, size_t len, log_entry_t *out)
{
    const char *p = line;
    const char *end = line + len;
    const char *q;

    /* Quitar salto de linea y retorno de carro del final. */
    while (end > p && (end[-1] == '\n' || end[-1] == '\r'))
        end--;

    /* 1. IP */
    q = skip_token(p, end);
    if (copy_field(out->ip, sizeof out->ip, p, q) < 0)
        return -1;
    p = q;

    /* 2 y 3. ident y usuario: se saltan. */
    for (int i = 0; i < 2; i++) {
        if (p >= end || *p != ' ')
            return -1;
        p++;
        q = skip_token(p, end);
        if (q == p)
            return -1;
        p = q;
    }

    /* 4. fecha entre corchetes. Solo se valida, no se usa todavia. */
    if (p >= end || *p != ' ')
        return -1;
    p++;
    if (p >= end || *p != '[')
        return -1;
    q = memchr(p, ']', (size_t)(end - p));
    if (!q)
        return -1;
    p = q + 1;

    /* 5. peticion entre comillas: METODO URL PROTOCOLO */
    if (p >= end || *p != ' ')
        return -1;
    p++;
    q = quoted_end(p, end);
    if (!q)
        return -1;
    {
        const char *r = p + 1;          /* despues de la comilla */
        const char *m_end = skip_token(r, q);
        if (copy_field(out->method, sizeof out->method, r, m_end) < 0)
            return -1;
        if (m_end >= q || *m_end != ' ')
            return -1;
        const char *u = m_end + 1;
        const char *u_end = skip_token(u, q);
        /* Cortar la cadena de consulta para el ranking. */
        const char *qm = memchr(u, '?', (size_t)(u_end - u));
        if (qm)
            u_end = qm;
        if (u >= u_end || *u != '/')
            return -1;
        if (copy_field(out->url, sizeof out->url, u, u_end) < 0) {
            /* URL muy larga: se trunca en vez de rechazar la linea. */
            memcpy(out->url, u, LP_URL_LEN - 1);
            out->url[LP_URL_LEN - 1] = '\0';
        }
    }
    p = q + 1;

    /* 6. codigo de estado */
    if (p >= end || *p != ' ')
        return -1;
    p++;
    {
        char *e;
        long st = strtol(p, &e, 10);
        if (e == p || st < 100 || st > 599 || (e < end && *e != ' '))
            return -1;
        out->status = (int)st;
        p = e;
    }

    /* 7. bytes, puede venir "-" */
    if (p >= end || *p != ' ')
        return -1;
    p++;
    if (p < end && *p == '-') {
        out->bytes = 0;
        p++;
    } else {
        char *e;
        long b = strtol(p, &e, 10);
        if (e == p || b < 0)
            return -1;
        out->bytes = b;
        p = e;
    }

    /* 8 y 9. referer y agente entre comillas. */
    for (int i = 0; i < 2; i++) {
        if (p >= end || *p != ' ')
            return -1;
        p++;
        q = quoted_end(p, end);
        if (!q)
            return -1;
        p = q + 1;
    }

    /* 10. tiempo de respuesta, opcional. */
    out->resp_time = 0.0;
    out->has_time = 0;
    while (p < end && *p == ' ')
        p++;
    if (p < end) {
        char *e;
        double t = strtod(p, &e);
        if (e == p || t < 0.0)
            return -1;
        p = e;
        while (p < end && *p == ' ')
            p++;
        if (p != end)
            return -1;      /* basura despues del tiempo */
        out->resp_time = t;
        out->has_time = 1;
    }
    return 0;
}
