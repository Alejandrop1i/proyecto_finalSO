# Guia para continuar LogPulse (Fase 2)

Oscar dejo la Fase 1 funcionando. Esta guia dice que hace cada uno, en que orden y
como probar. Fechas: entrega del repo e informe el 1 de diciembre antes de las 23:59,
sustentacion el 2 de diciembre.

## Antes de empezar (todos)

```bash
git clone <url del repo>
cd logpulse
sudo apt install build-essential libncurses-dev valgrind
make && make gen && make run
make probar
```

Si `make probar` dice OK, la base esta bien en tu maquina. Trabaja en una rama por
persona (`git checkout -b procesos`, `archivos`, `interfaz`) y haz commits pequenos y
frecuentes: el profesor revisa que todos tengan commits a lo largo del periodo.

Regla de oro: no modificar `bbuffer.c`, `parser.c` ni `metrics.c` salvo para corregir
un error. Ya pasan la prueba de consistencia y escalan con los hilos.

## Integrante de procesos e IPC

Objetivo: un proceso por archivo de log, metricas en memoria compartida, tuberia de
estado, deteccion de trabajador caido.

1. Crear `include/ipc.h` y `src/ipc.c` con:
   - `struct shm_layout`: un arreglo de `shm_slot` por trabajador, cada uno con los
     contadores de `metrics_t` que se muestran (total, mal formadas, codigos, bytes,
     top 10 ya calculado como arreglos fijos) y un `sem_t` con `sem_init(&s, 1, 1)`.
   - `ipc_create()` en el padre: `shm_open("/logpulse_<pid>", O_CREAT|O_RDWR, 0600)`,
     `ftruncate`, `mmap` con `MAP_SHARED`.
   - `ipc_attach()` no hace falta: los hijos heredan el mapa por `fork`.
   - `ipc_destroy()`: `munmap`, `shm_unlink`, `sem_destroy`. Debe correr siempre al salir,
     tambien con Ctrl+C. Evidencia de la demo: `ls /dev/shm` vacio al terminar.
2. Crear `src/coordinador.c`: por cada archivo `pipe()` + `fork()`. El hijo cierra el
   extremo de lectura, llama `worker_run()` y manda por la tuberia mensajes de texto
   cortos: `INICIO <pid> <archivo>`, `FIN <pid> <lineas>`, `ERROR <pid> <texto>`.
   El padre guarda los PIDs, y en un hilo de estado hace `poll()` sobre las tuberias.
3. `SIGCHLD` en el padre: manejador que solo pone una bandera; el hilo de estado hace
   `waitpid(-1, &st, WNOHANG)` en bucle y marca el trabajador como caido si
   `WIFSIGNALED(st)`. Evidencia de la demo: `kill -9` a un hijo y el panel lo muestra.
4. En `worker.c`, `publisher_thread()`: donde dice `TODO integrante de procesos`, copiar el
   snapshot al slot de este trabajador bajo `sem_wait`/`sem_post`.
5. Al terminar o con `SIGTERM`: el padre manda `SIGTERM` a los hijos, hace `waitpid` de
   todos, destruye la memoria compartida y cierra tuberias.

Pruebas: `./logpulse -m lote a.log b.log` con dos archivos, `ps -ef --forest` durante la
ejecucion, `ls /dev/shm` durante y despues, `valgrind --leak-check=full` sin fugas.

## Integrante de archivos y E/S de disco

Objetivo: `mmap` en modo lote con medicion contra `read`, modo vivo con `inotify` y
rotacion sin perder ni duplicar lineas.

1. En `worker.c`, `reader_thread()` tiene el `TODO`. Separar en dos lectores:
   - `reader_read()` el actual, deja igual.
   - `reader_mmap()`: `open`, `fstat` para el tamano, `mmap(PROT_READ, MAP_PRIVATE)`,
     `madvise(MADV_SEQUENTIAL)`. Recorrer el mapa cortando bloques de 64 KB en el ultimo
     `\n`. Para no copiar, agregar al `chunk_t` un campo `int owned` que diga si el
     parseador debe hacer `free()`. Al final `munmap`.
   - Seleccion con una opcion nueva `-l read|mmap` en `main.c` (por defecto mmap).
2. Medicion: `scripts/comparar_mmap.sh` corre ambos lectores 3 veces sobre el mismo
   archivo y guarda tiempo, `ru_minflt`, `ru_majflt` y `VmRSS` en `resultados/mmap.csv`.
   `main.c` ya imprime esos datos al final; solo hay que capturarlos con `grep`.
   Correr tambien con cache fria: `sync; echo 3 | sudo tee /proc/sys/vm/drop_caches`.
3. Modo vivo: `reader_live()` con `inotify_init1(IN_NONBLOCK)`,
   `inotify_add_watch(dir, IN_MODIFY|IN_MOVED_FROM|IN_CREATE)`. Se guarda el
   desplazamiento leido; con cada `IN_MODIFY` se hace `lseek` al desplazamiento y `read`
   de lo nuevo. Si llega `IN_MOVED_FROM` o `fstat` muestra otro inodo, es rotacion: leer lo
   que faltaba del descriptor viejo, cerrarlo, abrir el archivo nuevo y seguir desde 0.
   Evidencia de la demo: `./loggen -r 500 -o vivo.log --rotar 5000` en una terminal y
   `./logpulse -m vivo vivo.log` en otra; al final las lineas contadas deben ser las que
   loggen dice que escribio.
4. Mientras no haya datos nuevos, el lector espera en `poll()` sobre el descriptor de
   inotify, nunca con `sleep`.

## Integrante de entrada/salida y senales

Objetivo: panel ncurses que se refresca cada segundo, teclado no bloqueante, cierre
limpio, CSV, scripts e informe.

1. Crear `src/ui.c`: `initscr`, `cbreak`, `noecho`, `nodelay(stdscr, TRUE)`, `curs_set(0)`.
   Un bucle que cada segundo lee la memoria compartida (o, mientras el integrante de
   procesos termina, un `metrics_t` que le pase `worker_run`) y dibuja: resumen arriba,
   codigos HTTP, tres columnas de top 10, estado de cada trabajador (PID, lineas, vivo o
   caido), lineas mal formadas.
2. Teclas: `q` pone `g_stop = 1`; `p` alterna una bandera de pausa que solo congela el
   dibujo; `e` llama `metrics_export_csv()` con nombre `logpulse_<fecha>.csv` y muestra
   un aviso en el panel. `+` y `-` para la extension de hilos en caliente si queda tiempo.
3. `SIGWINCH`: redibujar al cambiar el tamano de la terminal. `endwin()` siempre al salir,
   incluido Ctrl+C. Evidencia de la demo: redimensionar la terminal y Ctrl+C sin dejar la
   terminal danada.
4. Agregar `-lncurses` a `LDLIBS` en el `Makefile`.
5. Informe tecnico (coordinar con todos): seguir la estructura de la seccion 8.2 del
   enunciado. Las graficas de rendimiento salen de `resultados/hilos.csv` (`make medir`
   en una maquina Linux, anotando `nproc`) y de `resultados/mmap.csv`.

## Lista de chequeo antes de etiquetar v1.0

- [ ] `make clean && make` sin advertencias.
- [ ] `make probar` dice OK.
- [ ] `valgrind --leak-check=full ./logpulse -m lote -t 2 access.log` sin fugas.
- [ ] `make tsan && ./logpulse_tsan -m lote -t 8 access.log` sin carreras.
- [ ] Modo vivo con `--rotar` sin perder ni duplicar lineas.
- [ ] `kill -9` a un trabajador se ve en el panel.
- [ ] `ls /dev/shm` vacio despues de salir.
- [ ] README con integrantes, extensiones y limitaciones.
- [ ] `git tag v1.0` sobre el commit entregado.
