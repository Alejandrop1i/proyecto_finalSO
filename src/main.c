/*
 * main.c - punto de entrada de logpulse.
 *
 * Uso: ./logpulse [-m lote|vivo] [-t N] [-e salida.csv] [-q] archivo1 [archivo2 ...]
 *
 * Fase 1 (este archivo): un solo trabajador dentro de este proceso, modo lote,
 * un archivo, metricas en texto plano al terminar.
 *
 * Fase 2 (integrante de procesos): aqui va el coordinador. Por cada archivo
 * hace fork(), el hijo llama worker_run(); el padre crea la memoria compartida
 * y el semaforo, lee la tuberia de estado y recoge a los hijos con waitpid().
 * Fase 2 (integrante de E/S): el panel ncurses reemplaza metrics_print y las
 * teclas q, p y e se leen con el teclado no bloqueante.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/resource.h>
#include "common.h"
#include "worker.h"

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;     /* solo se toca una bandera, nada mas es seguro aqui */
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;    /* sin SA_RESTART: read() vuelve con EINTR y el lector revisa g_stop */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-m lote|vivo] [-t N] [-e salida.csv] [-q] archivo [archivo2 ...]\n"
        "  -m MODO   lote (por defecto) o vivo\n"
        "  -t N      hilos parseadores por archivo, %d a %d (por defecto %d)\n"
        "  -e CSV    exportar las metricas a ese archivo al terminar\n"
        "  -q        sin linea de progreso\n",
        prog, LP_MIN_THREADS, LP_MAX_THREADS, LP_DEFAULT_THREADS);
}

/* Memoria residente del proceso leyendo /proc/self/status. -1 si no esta. */
static long vm_rss_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f)
        return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            kb = strtol(line + 6, NULL, 10);
            break;
        }
    }
    fclose(f);
    return kb;
}

static void print_resources(const worker_result_t *r)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    double user = (double)ru.ru_utime.tv_sec + (double)ru.ru_utime.tv_usec / 1e6;
    double sys  = (double)ru.ru_stime.tv_sec + (double)ru.ru_stime.tv_usec / 1e6;
    long rss = vm_rss_kb();

    printf("\nRecursos del proceso\n");
    printf("  CPU usuario:            %.3f s\n", user);
    printf("  CPU sistema:            %.3f s\n", sys);
    printf("  Fallos de pagina menores: %ld\n", ru.ru_minflt);
    printf("  Fallos de pagina mayores: %ld\n", ru.ru_majflt);
    printf("  Memoria residente max:  %ld KB\n", ru.ru_maxrss);
    if (rss >= 0)
        printf("  VmRSS actual:           %ld KB\n", rss);
    printf("  Bytes leidos:           %llu\n", r->bytes_read);
    printf("  Bloques por el bufer:   %lu\n", r->chunks);
    printf("  Esperas bufer lleno:    %lu\n", r->waits_full);
    printf("  Esperas bufer vacio:    %lu\n", r->waits_empty);
}

int main(int argc, char **argv)
{
    const char *mode = "lote";
    const char *csv  = NULL;
    int nthreads = LP_DEFAULT_THREADS;
    int quiet = 0;
    int opt;

    while ((opt = getopt(argc, argv, "m:t:e:qh")) != -1) {
        switch (opt) {
        case 'm': mode = optarg; break;
        case 't': nthreads = atoi(optarg); break;
        case 'e': csv = optarg; break;
        case 'q': quiet = 1; break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 2;
        }
    }
    if (optind >= argc) {
        usage(argv[0]);
        return 2;
    }
    if (nthreads < LP_MIN_THREADS || nthreads > LP_MAX_THREADS) {
        fprintf(stderr, "logpulse: -t debe estar entre %d y %d\n", LP_MIN_THREADS, LP_MAX_THREADS);
        return 2;
    }
    if (strcmp(mode, "lote") != 0 && strcmp(mode, "vivo") != 0) {
        fprintf(stderr, "logpulse: modo desconocido '%s'\n", mode);
        return 2;
    }
    if (strcmp(mode, "vivo") == 0) {
        fprintf(stderr, "logpulse: el modo vivo esta pendiente (Fase 2, inotify)\n");
        return 3;
    }
    if (argc - optind > 1) {
        fprintf(stderr, "logpulse: varios archivos esta pendiente (Fase 2, un proceso por archivo). "
                        "Se procesa solo %s\n", argv[optind]);
    }

    install_signals();

    worker_config_t cfg = { argv[optind], nthreads, quiet };
    worker_result_t res;
    int rc = worker_run(&cfg, &res);

    if (rc == 0 || res.metrics.total > 0) {
        if (g_stop)
            printf("(interrumpido por senal: metricas parciales)\n");
        metrics_print(&res.metrics, stdout, res.elapsed);
        print_resources(&res);
        /* Linea facil de leer por los scripts de medicion. */
        unsigned long lines = res.metrics.total + res.metrics.malformed;
        printf("RESUMEN hilos=%d lineas=%lu segundos=%.3f lineas_por_segundo=%.0f\n",
               nthreads, lines, res.elapsed,
               res.elapsed > 0 ? (double)lines / res.elapsed : 0.0);
        if (csv) {
            if (metrics_export_csv(&res.metrics, csv, res.elapsed) == 0)
                printf("CSV exportado a %s\n", csv);
            else
                fprintf(stderr, "logpulse: no se pudo escribir %s\n", csv);
        }
    }
    metrics_free(&res.metrics);
    return (rc == 0) ? 0 : 1;
}
