# LogPulse: analizador concurrente de logs de servidor web

Proyecto final de Sistemas Operativos, semestre 2026-2.
Tecnología en Sistematización de Datos, Universidad Distrital Francisco José de Caldas.

## Integrantes

| Integrante | Parte | Qué hace | Criterios de la rúbrica |
|---|---|---|---|
| (nombre) | Fase 1 (roles 1 y 2 del enunciado) | `loggen`, parseador, métricas, búfer acotado, hilos parseadores, terminación ordenada, `Makefile`, scripts de prueba y medición | Revisión de avance, Hilos y sincronización |
| (nombre) | Procesos e IPC (rol 3) | `fork` por archivo, memoria compartida con `shm_open` y semáforo, tubería de estado, `SIGCHLD`, detección de trabajador caído | Procesos y comunicación |
| (nombre) | Archivos (primera mitad del rol 4) | `mmap` en modo lote, comparación contra `read` con tiempo y fallos de página, `inotify`, rotación, modo vivo | Memoria, Archivos |
| (nombre) | Interfaz e informe (segunda mitad del rol 4) | Panel ncurses, teclas `q` `p` `e`, exportación CSV, cierre limpio de terminal, armado del informe técnico en PDF | Entrada/salida y señales, Informe técnico |

La funcionalidad del núcleo es responsabilidad de todos: se cumple cuando las cuatro partes
funcionan juntas en la sustentación.

El detalle de qué toca a cada uno y por dónde engancha cada módulo está en
[docs/arquitectura.md](docs/arquitectura.md) y [docs/guia_companeros.md](docs/guia_companeros.md).

## Compilación

Requiere Linux (Debian 12 o Ubuntu 22.04/24.04), `gcc`, `make` y `libncurses-dev`.

```bash
sudo apt install build-essential libncurses-dev valgrind
make            # compila logpulse y loggen sin advertencias
make gen        # genera access.log con 1 000 000 de líneas
make run        # ./logpulse -m lote -t 4 access.log
make probar     # verifica que logpulse cuenta lo mismo que escribió loggen
make medir      # rendimiento con 1, 2, 4 y 8 hilos -> resultados/hilos.csv
make tsan       # binario con el sanitizador de hilos
make clean
```

## Uso

```
./logpulse [-m lote|vivo] [-t N] [-e salida.csv] [-q] archivo [archivo2 ...]
   -m   modo: lote (por defecto) o vivo
   -t   hilos parseadores por archivo, de 1 a 16, por defecto 4
   -e   exporta las métricas a un CSV al terminar
   -q   sin la línea de progreso por segundo

./loggen [-n LINEAS] [-r TASA] [-o ARCHIVO] [-s SEMILLA] [-b PCT] [--rotar N]
   -n   cuántas líneas escribir y salir
   -r   líneas por segundo (modo en vivo, hasta Ctrl+C)
   -o   archivo de salida (por defecto la salida estándar)
   -s   semilla, para repetir exactamente el mismo log
   -b   porcentaje de líneas mal formadas, por defecto 1
   --rotar N   cada N líneas renombra el archivo a ARCHIVO.1 y abre uno nuevo
```

Ejemplos:

```bash
./loggen -n 1000000 -o access.log            # archivo de prueba
./logpulse -m lote -t 8 access.log           # modo lote
./loggen -r 500 -o vivo.log --rotar 5000     # escritor para el modo en vivo
```

## Estado actual

Fase 1 completa (lo exigido en la revisión de avance, sección 7.1 del enunciado):

- `loggen` genera el archivo de 1 000 000 de líneas en unos 3 segundos.
- Un proceso con hilo lector, búfer acotado de 64 bloques de 64 KB, N parseadores
  y un hilo publicador. Sincronización con mutex y variables de condición,
  terminación ordenada con píldora venenosa.
- Métricas mínimas en texto plano: total, peticiones por segundo, códigos
  2xx a 5xx, bytes, top 10 IPs, top 10 URLs, top 10 URLs más lentas, líneas
  mal formadas contadas y mostradas.
- Exportación a CSV con `-e`.
- `SIGINT` y `SIGTERM` terminan de forma ordenada e imprimen métricas parciales.
- Reporte de `getrusage` y `VmRSS` al final.

Pendiente (Fase 2):

- Modo `vivo` con `inotify` y rotación.
- `mmap` en modo lote y comparación con `read`.
- Un proceso por archivo con `fork`, memoria compartida, semáforo y tubería.
- Panel ncurses con teclas `q`, `p`, `e`.

## Extensiones implementadas

Ninguna todavía.

## Limitaciones conocidas

- Una línea de más de 64 KB sin salto de línea se recorta.
- La fecha de cada línea se valida pero no se interpreta.
- Las claves de IP y URL se guardan en tablas hash propias con un tope de
  255 caracteres por URL. Las URLs más largas se truncan.
