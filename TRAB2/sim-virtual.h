/*
 * sim-virtual.h
 * Aline Jéssica David Gonçalves, 2320276
 *
 * Cada quadro físico guarda o que o enunciado pede (bits R e M e o
 * instante do último acesso). load_time distingue a ordem de carga
 * (FIFO e desempate do NRU). next_use guarda o próximo acesso futuro
 * à página que ocupa o quadro (algoritmo Ótimo).
 */

#ifndef SIM_VIRTUAL_H
#define SIM_VIRTUAL_H

#include <stdint.h>

/* A cada NRU_TICK acessos os bits R são zerados, como na interrupção
 * de relógio descrita pelo NRU (Tanenbaum): a classe da página passa a
 * refletir só o passado recente. */
#define NRU_TICK 1000ull

typedef enum {
    ALG_LRU = 0,
    ALG_NRU,
    ALG_FIFO,
    ALG_OPT
} Algoritmo;

typedef struct {
    unsigned pagina;
    int R;
    int M;
    unsigned long long ultimo_acesso;
    unsigned long long instante_carga;
    unsigned long long proximo_uso;
} Quadro;

#endif
