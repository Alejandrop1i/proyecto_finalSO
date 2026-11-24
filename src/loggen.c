/*
 * loggen.c - generador de lineas de log sinteticas en formato combinado.
 *
 * Uso:
 *   ./loggen -n 1000000 -o access.log          genera 1 000 000 de lineas y sale
 *   ./loggen -r 500 -o access.log              modo vivo: 500 lineas/s hasta Ctrl+C
 *   ./loggen -r 500 -o access.log --rotar 5000 cada 5000 lineas rota el archivo
 *
 * Opciones:
 *   -n N        cuantas lineas escribir (0 = sin limite, por defecto 0 si hay -r)
 *   -r TASA     lineas por segundo (0 = lo mas rapido posible)
 *   -o ARCHIVO  archivo de salida (por defecto la salida estandar)
 *   -s SEMILLA  semilla del generador, para repetir el mismo log
 *   -b PCT      porcentaje de lineas mal formadas (por defecto 1)
 *   --rotar N   renombra el archivo a ARCHIVO.1 y abre uno nuevo cada N lineas
 *
 * Al terminar imprime en stderr cuantas lineas escribio, cuantas mal formadas
 * y cuantas sin tiempo, para comparar contra lo que reporta logpulse.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>

#define OUT_BUF (64 * 1024)
#define N_IPS   60

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ---------- aleatorio reproducible (xorshift64) ---------- */
static unsigned long long g_rng = 88172645463325252ULL;

static unsigned long long rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static double rnd01(void)
{
    return (double)(rnd() >> 11) / 9007199254740992.0;
}

static int rnd_int(int n)
{
    return (int)(rnd() % (unsigned long long)n);
}

/* ---------- datos de prueba ---------- */

/* URLs con un tiempo base distinto para que el ranking de lentas sea claro. */
static const struct {
    const char *path;
    double      base_time;
    int         weight;
} URLS[] = {
    { "/",                        0.010, 20 },
    { "/index.html",              0.012, 15 },
    { "/login",                   0.080, 10 },
    { "/logout",                  0.020,  3 },
    { "/api/notas",               0.090, 12 },
    { "/api/notas/detalle",       0.150,  6 },
    { "/api/estudiantes",         0.120,  8 },
    { "/api/materias",            0.070,  7 },
    { "/api/horarios",            0.060,  6 },
    { "/api/reportes/semestre",   1.800,  2 },
    { "/api/reportes/promedios",  2.500,  1 },
    { "/api/buscar",              0.300,  5 },
    { "/api/exportar/csv",        1.200,  2 },
    { "/api/exportar/pdf",        3.200,  1 },
    { "/static/app.js",           0.005, 14 },
    { "/static/app.css",          0.004, 14 },
    { "/static/logo.png",         0.003, 10 },
    { "/static/fuentes/ud.woff2", 0.004,  5 },
    { "/img/banner.jpg",          0.008,  6 },
    { "/img/campus.jpg",          0.009,  4 },
    { "/favicon.ico",             0.002, 10 },
    { "/admin",                   0.050,  3 },
    { "/admin/usuarios",          0.200,  2 },
    { "/admin/config",            0.180,  1 },
    { "/perfil",                  0.060,  5 },
    { "/perfil/editar",           0.110,  2 },
    { "/foro",                    0.090,  4 },
    { "/foro/tema",               0.130,  4 },
    { "/foro/responder",          0.160,  2 },
    { "/descargas/guia.pdf",      0.700,  2 },
    { "/descargas/syllabus.pdf",  0.650,  2 },
    { "/wp-login.php",            0.030,  2 },
    { "/.env",                    0.010,  1 },
    { "/robots.txt",              0.002,  3 },
    { "/sitemap.xml",             0.015,  1 },
    { "/api/salud",               0.001,  8 },
    { "/api/version",             0.001,  3 },
    { "/ayuda",                   0.040,  3 },
    { "/contacto",                0.045,  2 },
    { "/noticias",                0.085,  4 },
};
#define N_URLS ((int)(sizeof URLS / sizeof URLS[0]))

static const char *AGENTS[] = {
    "Mozilla/5.0",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/130.0",
    "Mozilla/5.0 (X11; Linux x86_64) Firefox/132.0",
    "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0) Safari/605.1",
    "curl/8.5.0",
    "python-requests/2.32",
};
#define N_AGENTS ((int)(sizeof AGENTS / sizeof AGENTS[0]))

static const char *REFERERS[] = { "-", "-", "-", "https://www.udistrital.edu.co/",
                                  "https://www.google.com/", "https://cursos.udistrital.edu.co/" };
#define N_REFERERS ((int)(sizeof REFERERS / sizeof REFERERS[0]))

static const char *QUERIES[] = { "", "", "", "?id=42", "?page=2", "?q=sistemas", "?id=7&tab=notas" };
#define N_QUERIES ((int)(sizeof QUERIES / sizeof QUERIES[0]))

static char g_ips[N_IPS][16];
static int  g_url_weights_total = 0;

static void init_pools(void)
{
    for (int i = 0; i < N_IPS; i++) {
        int kind = rnd_int(3);
        if (kind == 0)
            snprintf(g_ips[i], sizeof g_ips[i], "192.168.%d.%d", rnd_int(20), 1 + rnd_int(254));
        else if (kind == 1)
            snprintf(g_ips[i], sizeof g_ips[i], "10.%d.%d.%d", rnd_int(256), rnd_int(256), 1 + rnd_int(254));
        else
            snprintf(g_ips[i], sizeof g_ips[i], "181.%d.%d.%d", 50 + rnd_int(100), rnd_int(256), 1 + rnd_int(254));
    }
    for (int i = 0; i < N_URLS; i++)
        g_url_weights_total += URLS[i].weight;
}

/* Las IPs de indice bajo salen mucho mas que las demas (distribucion sesgada). */
static const char *pick_ip(void)
{
    double r = rnd01();
    int idx = (int)(r * r * N_IPS);
    if (idx >= N_IPS)
        idx = N_IPS - 1;
    return g_ips[idx];
}

static int pick_url(void)
{
    int r = rnd_int(g_url_weights_total);
    for (int i = 0; i < N_URLS; i++) {
        r -= URLS[i].weight;
        if (r < 0)
            return i;
    }
    return 0;
}

static const char *pick_method(void)
{
    int r = rnd_int(100);
    if (r < 85) return "GET";
    if (r < 95) return "POST";
    if (r < 98) return "PUT";
    return "DELETE";
}

static int pick_status(void)
{
    int r = rnd_int(1000);
    if (r < 800) return 200;
    if (r < 830) return 201;
    if (r < 850) return 204;
    if (r < 870) return 301;
    if (r < 900) return 302;
    if (r < 910) return 304;
    if (r < 950) return 404;
    if (r < 965) return 403;
    if (r < 975) return 401;
    if (r < 990) return 500;
    if (r < 997) return 503;
    return 502;
}

/* ---------- salida con bufer y escritura completa ---------- */

typedef struct {
    int    fd;
    char   buf[OUT_BUF];
    size_t len;
} out_t;

static int write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int out_flush(out_t *o)
{
    if (o->len == 0)
        return 0;
    int rc = write_all(o->fd, o->buf, o->len);
    o->len = 0;
    return rc;
}

static int out_put(out_t *o, const char *s, size_t n)
{
    if (o->len + n > sizeof o->buf) {
        if (out_flush(o) < 0)
            return -1;
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    return 0;
}

/* ---------- generacion de una linea ---------- */

static void format_time(char *dst, size_t cap, time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(dst, cap, "%d/%b/%Y:%H:%M:%S %z", &tm);
}

/* Devuelve la longitud de la linea escrita en dst. */
static size_t make_line(char *dst, size_t cap, time_t ts, int bad_pct,
                        int *is_bad, int *no_time)
{
    char   when[64];
    int    u = pick_url();
    int    st = pick_status();
    long   bytes;
    double t = URLS[u].base_time * (0.6 + 0.8 * rnd01()) + rnd01() * 0.01;
    size_t n;

    *is_bad = 0;
    *no_time = 0;
    format_time(when, sizeof when, ts);

    if (st == 304)
        bytes = 0;
    else if (st >= 500)
        bytes = 200 + rnd_int(400);
    else
        bytes = 100 + rnd_int(50000);

    if (rnd_int(100) < bad_pct) {
        *is_bad = 1;
        switch (rnd_int(4)) {
        case 0:   /* linea cortada a la mitad */
            n = (size_t)snprintf(dst, cap, "%s - - [%s] \"GET %s HT\n", pick_ip(), when, URLS[u].path);
            break;
        case 1:   /* falta la comilla de cierre de la peticion */
            n = (size_t)snprintf(dst, cap, "%s - - [%s] \"%s %s HTTP/1.1 %d %ld \"-\" \"%s\" %.3f\n",
                                 pick_ip(), when, pick_method(), URLS[u].path, st, bytes,
                                 AGENTS[rnd_int(N_AGENTS)], t);
            break;
        case 2:   /* codigo de estado que no es numero */
            n = (size_t)snprintf(dst, cap, "%s - - [%s] \"GET %s HTTP/1.1\" ERR %ld \"-\" \"Mozilla/5.0\" %.3f\n",
                                 pick_ip(), when, URLS[u].path, bytes, t);
            break;
        default:  /* basura */
            n = (size_t)snprintf(dst, cap, "### linea corrupta %llu ###\n", rnd());
            break;
        }
        return n;
    }

    int with_time = rnd_int(100) >= 5;      /* 5 % sin tiempo */
    if (!with_time)
        *no_time = 1;

    if (bytes == 0 && st == 304) {
        n = (size_t)snprintf(dst, cap, "%s - - [%s] \"%s %s%s HTTP/1.1\" %d - \"%s\" \"%s\"",
                             pick_ip(), when, pick_method(), URLS[u].path, QUERIES[rnd_int(N_QUERIES)],
                             st, REFERERS[rnd_int(N_REFERERS)], AGENTS[rnd_int(N_AGENTS)]);
    } else {
        n = (size_t)snprintf(dst, cap, "%s - - [%s] \"%s %s%s HTTP/1.1\" %d %ld \"%s\" \"%s\"",
                             pick_ip(), when, pick_method(), URLS[u].path, QUERIES[rnd_int(N_QUERIES)],
                             st, bytes, REFERERS[rnd_int(N_REFERERS)], AGENTS[rnd_int(N_AGENTS)]);
    }
    if (with_time)
        n += (size_t)snprintf(dst + n, cap - n, " %.3f", t);
    dst[n++] = '\n';
    dst[n] = '\0';
    return n;
}

/* ---------- archivo de salida y rotacion ---------- */

static int open_output(const char *path)
{
    if (!path)
        return STDOUT_FILENO;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        fprintf(stderr, "loggen: no se pudo abrir %s: %s\n", path, strerror(errno));
    return fd;
}

/* Rota: ARCHIVO -> ARCHIVO.1 y abre un ARCHIVO nuevo vacio. */
static int rotate(out_t *o, const char *path)
{
    char old[4096];
    if (out_flush(o) < 0)
        return -1;
    snprintf(old, sizeof old, "%s.1", path);
    unlink(old);
    if (rename(path, old) < 0) {
        fprintf(stderr, "loggen: no se pudo rotar %s: %s\n", path, strerror(errno));
        return -1;
    }
    close(o->fd);
    o->fd = open_output(path);
    if (o->fd < 0)
        return -1;
    fprintf(stderr, "loggen: rotado %s -> %s\n", path, old);
    return 0;
}

/* Duerme hasta un instante absoluto del reloj monotono. */
static void sleep_until(const struct timespec *deadline)
{
    struct timespec now, rem;
    clock_gettime(CLOCK_MONOTONIC, &now);
    rem.tv_sec  = deadline->tv_sec - now.tv_sec;
    rem.tv_nsec = deadline->tv_nsec - now.tv_nsec;
    if (rem.tv_nsec < 0) {
        rem.tv_sec--;
        rem.tv_nsec += 1000000000L;
    }
    if (rem.tv_sec < 0)
        return;
    while (nanosleep(&rem, &rem) < 0 && errno == EINTR && !g_stop)
        ;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Uso: %s [-n LINEAS] [-r TASA] [-o ARCHIVO] [-s SEMILLA] [-b PCT] [--rotar N]\n", prog);
}

int main(int argc, char **argv)
{
    long        n_lines = -1;
    long        rate    = 0;
    const char *path    = NULL;
    int         bad_pct = 1;
    long        rotate_every = 0;
    unsigned long long seed = 20262ULL;

    static const struct option longopts[] = {
        { "rotar", required_argument, NULL, 'R' },
        { "help",  no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "n:r:o:s:b:h", longopts, NULL)) != -1) {
        switch (opt) {
        case 'n': n_lines = atol(optarg); break;
        case 'r': rate = atol(optarg); break;
        case 'o': path = optarg; break;
        case 's': seed = strtoull(optarg, NULL, 10); break;
        case 'b': bad_pct = atoi(optarg); break;
        case 'R': rotate_every = atol(optarg); break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 2;
        }
    }
    if (n_lines < 0 && rate <= 0) {
        fprintf(stderr, "loggen: indique -n LINEAS o -r TASA\n");
        usage(argv[0]);
        return 2;
    }
    if (rotate_every > 0 && !path) {
        fprintf(stderr, "loggen: --rotar necesita -o ARCHIVO\n");
        return 2;
    }
    if (bad_pct < 0 || bad_pct > 100)
        bad_pct = 1;

    g_rng = seed ? seed : 1;
    init_pools();

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    out_t out;
    out.len = 0;
    out.fd = open_output(path);
    if (out.fd < 0)
        return 1;

    /* En modo rapido las fechas se simulan a 200 peticiones por segundo
     * empezando en el pasado; en modo con tasa se usa la hora real. */
    time_t now = time(NULL);
    time_t base = (rate > 0 || n_lines <= 0) ? now : now - n_lines / 200;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    long batch = (rate > 0) ? (rate / 20 > 0 ? rate / 20 : 1) : 1;

    long written = 0, bad = 0, no_time = 0, since_rotate = 0;
    char line[1024];

    while (!g_stop && (n_lines < 0 || written < n_lines)) {
        if (rate > 0) {
            /* Espera hasta el instante en que debe salir la linea 'written'. */
            double due = (double)written / (double)rate;
            struct timespec deadline = start;
            deadline.tv_sec  += (time_t)due;
            deadline.tv_nsec += (long)((due - (double)(time_t)due) * 1e9);
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_sec++;
                deadline.tv_nsec -= 1000000000L;
            }
            sleep_until(&deadline);
            if (g_stop)
                break;
        }
        long todo = batch;
        if (n_lines >= 0 && written + todo > n_lines)
            todo = n_lines - written;
        for (long i = 0; i < todo; i++) {
            int is_bad, nt;
            time_t ts = (rate > 0) ? time(NULL) : base + written / 200;
            size_t n = make_line(line, sizeof line, ts, bad_pct, &is_bad, &nt);
            if (out_put(&out, line, n) < 0) {
                fprintf(stderr, "loggen: error de escritura: %s\n", strerror(errno));
                goto done;
            }
            written++;
            bad += is_bad;
            no_time += nt;
            since_rotate++;
            if (rotate_every > 0 && since_rotate >= rotate_every) {
                if (rotate(&out, path) < 0)
                    goto done;
                since_rotate = 0;
            }
        }
        if (rate > 0)
            out_flush(&out);    /* en modo vivo cada lote llega al archivo de inmediato */
    }

done:
    out_flush(&out);
    if (out.fd != STDOUT_FILENO)
        close(out.fd);
    fprintf(stderr, "loggen: %ld lineas escritas, %ld mal formadas, %ld sin tiempo\n",
            written, bad, no_time);
    return 0;
}
