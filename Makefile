# LogPulse - analizador concurrente de logs de servidor web
# Objetivos: all, clean, run, gen, medir, probar, tsan

CC      = gcc
CFLAGS  = -std=c11 -O2 -Wall -Wextra -pthread -D_POSIX_C_SOURCE=200809L -Iinclude
LDFLAGS = -pthread
# Cuando el integrante de E/S agregue el panel ncurses: LDLIBS += -lncurses
LDLIBS  = -lrt

SRC_LP   = src/main.c src/worker.c src/bbuffer.c src/metrics.c src/parser.c src/common.c
SRC_GEN  = src/loggen.c
OBJ_LP   = $(SRC_LP:.c=.o)
OBJ_GEN  = $(SRC_GEN:.c=.o)

LOG     ?= access.log
HILOS   ?= 4

.PHONY: all clean run gen medir probar tsan

all: logpulse loggen

logpulse: $(OBJ_LP)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

loggen: $(OBJ_GEN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.c include/*.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Genera el log de prueba de 1 000 000 de lineas.
gen: loggen
	./loggen -n 1000000 -o $(LOG)

run: logpulse
	./logpulse -m lote -t $(HILOS) $(LOG)

# Rendimiento con 1, 2, 4 y 8 hilos -> resultados/hilos.csv
medir: logpulse
	bash scripts/medir_hilos.sh $(LOG)

# Prueba rapida: compara lo que escribio loggen con lo que conto logpulse.
probar: all
	bash scripts/probar.sh

# Binario con el sanitizador de hilos para buscar condiciones de carrera.
tsan:
	$(CC) -std=c11 -O1 -g -Wall -Wextra -pthread -fsanitize=thread -D_POSIX_C_SOURCE=200809L -Iinclude \
	    -o logpulse_tsan $(SRC_LP) $(LDLIBS)

clean:
	rm -f logpulse loggen logpulse_tsan src/*.o
