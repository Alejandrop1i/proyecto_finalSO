# Arquitectura de LogPulse y reparto de tareas

## Diagrama

```
                         Proceso coordinador: logpulse (main.c)
 +----------------------------------------------------------------------------+
 |  Hilo de estado            Manejo de senales          Hilo de interfaz      |
 |  lee tuberia + waitpid()   SIGINT SIGTERM SIGCHLD     panel ncurses, teclas |
 |         ^                                                      ^            |
 |         | tuberia de estado                                    | lee        |
 |         |                       +-----------------------------------------+ |
 |         |         fork()        |  Memoria compartida (shm_open + mmap)   | |
 |         |           |           |  protegida por semaforo POSIX pshared   | |
 +---------|-----------|-----------+--------------------^--------------------+-+
           |           v                                | publica
 +---------|------------------------------------------------------------------+
 |  Proceso trabajador 1 (worker.c)   lee access1.log     |                   |
 |                                                        |                   |
 |  Hilo lector  --> [ Bufer acotado ] --> Parseadores xN --> Hilo publicador |
 |  read()/mmap      64 bloques 64 KB      parser.c        fusiona metricas   |
 |  inotify          mutex + 2 cond        metrics.c       cada segundo       |
 |                   pildora venenosa      (local por hilo)                    |
 +----------------------------------------------------------------------------+
 |  Trabajadores 2 ... N: misma estructura, un proceso por archivo de log     |
 +----------------------------------------------------------------------------+
```

## Modulos y quien los hace

| Archivo | Que hace | Estado | Responsable |
|---|---|---|---|
| `src/loggen.c` | Genera lineas sinteticas, tasa configurable, rotacion | Hecho | Oscar |
| `src/parser.c` | Extrae IP, metodo, URL, codigo, bytes, tiempo de una linea | Hecho | Oscar |
| `src/metrics.c` | Tablas hash de IPs y URLs, top 10, reporte en texto y CSV | Hecho | Oscar |
| `src/bbuffer.c` | Bufer circular acotado con mutex y variables de condicion | Hecho | Oscar |
| `src/worker.c` | Hilo lector, N parseadores, publicador, terminacion ordenada | Hecho (Fase 1) | Oscar |
| `src/main.c` | Argumentos, senales, un trabajador en proceso | Hecho (Fase 1) | Oscar |
| `Makefile`, `scripts/` | Compilacion, generacion, medicion, prueba | Hecho | Oscar |
| `src/ipc.c` (nuevo) | `shm_open`, `ftruncate`, `mmap`, `sem_open`, tuberia | Pendiente | Integrante procesos |
| `src/coordinador.c` (nuevo) | `fork` por archivo, `waitpid`, `SIGCHLD`, trabajador caido | Pendiente | Integrante procesos |
| `src/reader_mmap.c` (nuevo) | Lector con `mmap` + `madvise`, medicion contra `read` | Pendiente | Integrante archivos |
| `src/reader_live.c` (nuevo) | `inotify`, solo lo nuevo, deteccion de rotacion por inodo | Pendiente | Integrante archivos |
| `src/ui.c` (nuevo) | Panel ncurses, `nodelay`, teclas q p e, redimensionar | Pendiente | Integrante E/S |
| `scripts/comparar_mmap.sh` | Tiempo y fallos de pagina `mmap` vs `read` | Pendiente | Integrante archivos |
| Informe tecnico PDF | Seccion 8.2 del enunciado | Pendiente | Todos, coordina integrante E/S |

## Puntos de enganche

Lo que ya existe no se reescribe. Cada integrante conecta su modulo en un punto concreto:

1. **Integrante de procesos.** En `main.c`, donde hoy se llama `worker_run()` una sola vez,
   va el coordinador: por cada archivo `fork()`, el hijo llama `worker_run()` con su archivo.
   En `worker.c`, la funcion `publisher_thread()` tiene un `TODO`: en vez de imprimir en
   `stderr`, copia el `snapshot` a la memoria compartida bajo el semaforo y escribe un
   mensaje de estado en la tuberia. El padre recoge hijos con `waitpid()` desde el manejador
   de `SIGCHLD` o desde el hilo de estado.

2. **Integrante de archivos.** En `worker.c`, la funcion `reader_thread()` tiene un `TODO`.
   El contrato es simple: producir `chunk_t` con lineas completas y meterlos con
   `bbuffer_push()`, y al final meter `nthreads` pildoras. Con `mmap` los bloques pueden
   apuntar al mapa sin copiar, pero entonces el parseador no debe hacer `free()` del bloque
   (agregar una bandera al `chunk_t`). Para el modo vivo, `inotify` avisa cuando hay bytes
   nuevos y se lee desde el ultimo desplazamiento; si cambia el inodo (`fstat`) es rotacion:
   terminar de leer el archivo viejo y abrir el nuevo desde cero.

3. **Integrante de E/S.** Reemplaza `metrics_print()` al final por un panel que se refresca
   cada segundo leyendo la memoria compartida. `metrics_export_csv()` ya existe y es lo que
   dispara la tecla `e`. La tecla `p` solo congela el refresco del panel. La tecla `q` pone
   `g_stop = 1` igual que `SIGINT`, y todo lo demas ya termina ordenado.

## Sincronizacion: secciones criticas identificadas

| Seccion critica | Proteccion | Donde |
|---|---|---|
| Insertar y sacar bloques del bufer | `mtx` + `not_full` + `not_empty` | `bbuffer.c` |
| Metricas locales de cada parseador | un mutex por parseador, bloqueado por bloque | `worker.c` |
| Aviso de fin al publicador | `pub_mtx` + `pub_cv` con `pthread_cond_timedwait` | `worker.c` |
| Bandera de parada por senal | `volatile sig_atomic_t g_stop` | `common.c` |
| Metricas en memoria compartida (Fase 2) | semaforo POSIX con `pshared=1` | `ipc.c` |

No hay espera activa: toda espera es `pthread_cond_wait` o `pthread_cond_timedwait`.
No hay interbloqueo porque ningun hilo toma dos mutex a la vez, salvo el publicador, que
toma los mutex locales de uno en uno y nunca mientras tiene `pub_mtx`.
