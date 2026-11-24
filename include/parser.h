/*
 * parser.h - extraccion de campos de una linea de log en formato combinado.
 *
 * Linea esperada (seccion 10 del enunciado):
 *   IP - - [fecha] "METODO /url?consulta HTTP/1.1" codigo bytes "referer" "agente" tiempo
 * El campo tiempo al final es opcional. La consulta (?...) se quita de la URL.
 */
#ifndef LP_PARSER_H
#define LP_PARSER_H

#include <stddef.h>
#include "common.h"

typedef struct {
    char   ip[LP_IP_LEN];
    char   method[LP_METHOD_LEN];
    char   url[LP_URL_LEN];
    int    status;      /* codigo HTTP, 100..599 */
    long   bytes;       /* bytes transferidos, 0 si venia "-" */
    double resp_time;   /* tiempo de respuesta en segundos */
    int    has_time;    /* 1 si la linea traia el campo tiempo */
} log_entry_t;

/*
 * Parsea una linea (sin necesidad de terminador nulo, se usa len).
 * Devuelve 0 si la linea esta bien formada y -1 si no.
 */
int parse_line(const char *line, size_t len, log_entry_t *out);

#endif
