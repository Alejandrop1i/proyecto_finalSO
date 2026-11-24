/*
 * common.h - constantes y utilidades compartidas por todo LogPulse.
 */
#ifndef LP_COMMON_H
#define LP_COMMON_H

#include <stddef.h>
#include <signal.h>

/* Tamano de cada bloque que el hilo lector deja en el bufer acotado. */
#define LP_CHUNK_SIZE      (64 * 1024)
/* Cuantos bloques caben en el bufer acotado. */
#define LP_BUFFER_SLOTS    64
/* Limites de hilos parseadores segun el enunciado. */
#define LP_MIN_THREADS     1
#define LP_MAX_THREADS     16
#define LP_DEFAULT_THREADS 4

/* Tamanos de campos del parseador. */
#define LP_IP_LEN      46
#define LP_METHOD_LEN  8
#define LP_URL_LEN     256

/* Cuantas lineas mal formadas se guardan de muestra para mostrarlas. */
#define LP_BAD_SAMPLES 5
#define LP_BAD_SAMPLE_LEN 120

/* Bandera global que ponen en 1 los manejadores de SIGINT y SIGTERM. */
extern volatile sig_atomic_t g_stop;

/* Segundos desde un reloj monotono, para medir tiempos. */
double lp_now(void);

#endif
