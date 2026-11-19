/*
 * sim-virtual.c
 * INF1316 — Sistemas Operacionais — Trabalho 2 (2026.2)
 * Aline Jéssica David Gonçalves, 2320276
 *
 * Simula paginação sob demanda para um único processo e substitui
 * páginas com LRU, NRU, FIFO ou Ótimo (Belady).
 *
 * Uso:
 *   sim-virtual <LRU|NRU|FIFO|Otimo> <arquivo.log> <4|8> <2|4>
 *
 * O terceiro argumento é o tamanho da página em KB; o quarto é a
 * memória física em MB. Exemplo: sim-virtual LRU arquivo.log 8 2
 *
 * Página lógica: page = addr >> s, com s = log2(tamanho da página).
 * Falta de página conta toda carga, inclusive as compulsórias.
 * Página suja só incrementa o contador quando é substituída.
 * Páginas ainda sujas no fim da execução não são escritas.
 */

#define _CRT_SECURE_NO_WARNINGS

#include "sim-virtual.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define NUNCA_MAIS (~0ull)

typedef struct {
    unsigned pagina;
    char rw;
} Acesso;

static void uso(void) {
    fprintf(stderr,
            "Uso: sim-virtual <LRU|NRU|FIFO|Otimo> <arquivo.log> <pagina_KB> <memoria_MB>\n"
            "  pagina_KB : 4 ou 8\n"
            "  memoria_MB: 2 ou 4\n"
            "Exemplo: sim-virtual LRU arquivo.log 8 2\n");
}

/* Aceita "Otimo", "Ótimo" e "opt", sem diferenciar maiúsculas. */
static int reconhecer_algoritmo(const char *texto, Algoritmo *alg) {
    char norm[32];
    size_t j = 0;

    for (size_t i = 0; texto[i] != '\0' && j + 1 < sizeof norm;) {
        unsigned char c = (unsigned char)texto[i];
        unsigned char seguinte = (unsigned char)texto[i + 1];
        /* Ó/ó em UTF-8 (C3 93 / C3 B3) ou em Windows-1252 (D3 / F3). */
        if (c == 0xC3 && (seguinte == 0x93 || seguinte == 0xB3)) {
            norm[j++] = 'o';
            i += 2;
            continue;
        }
        if (c == 0xD3 || c == 0xF3) {
            norm[j++] = 'o';
            i++;
            continue;
        }
        if (c >= 'A' && c <= 'Z')
            c = (unsigned char)(c - 'A' + 'a');
        if (c >= 'a' && c <= 'z')
            norm[j++] = (char)c;
        i++;
    }
    norm[j] = '\0';

    if (strcmp(norm, "lru") == 0) {
        *alg = ALG_LRU;
        return 1;
    }
    if (strcmp(norm, "nru") == 0) {
        *alg = ALG_NRU;
        return 1;
    }
    if (strcmp(norm, "fifo") == 0) {
        *alg = ALG_FIFO;
        return 1;
    }
    if (strcmp(norm, "otimo") == 0 || strcmp(norm, "opt") == 0) {
        *alg = ALG_OPT;
        return 1;
    }
    return 0;
}

static const char *nome_algoritmo(Algoritmo alg) {
    switch (alg) {
    case ALG_LRU:
        return "LRU";
    case ALG_NRU:
        return "NRU";
    case ALG_FIFO:
        return "FIFO";
    case ALG_OPT:
        return "Ótimo";
    }
    return "?";
}

static int deslocamento_pagina(int pagina_kb) {
    unsigned bytes = (unsigned)pagina_kb * 1024u;
    int s = 0;
    while (bytes > 1u) {
        bytes >>= 1;
        s++;
    }
    return s;
}

static Acesso *ler_acessos(const char *caminho, int s, size_t *quantidade) {
    FILE *arquivo = fopen(caminho, "r");
    Acesso *acessos;
    size_t n = 0;
    size_t cap = 1024;
    char linha[128];

    if (!arquivo) {
        fprintf(stderr, "Nao foi possivel abrir '%s'.\n", caminho);
        return NULL;
    }

    acessos = malloc(cap * sizeof(*acessos));
    if (!acessos) {
        fprintf(stderr, "Memoria insuficiente para ler o log.\n");
        fclose(arquivo);
        return NULL;
    }

    while (fgets(linha, sizeof linha, arquivo)) {
        unsigned addr;
        char rw;
        if (sscanf(linha, "%x %c", &addr, &rw) != 2)
            continue;
        rw = (char)toupper((unsigned char)rw);
        if (rw != 'R' && rw != 'W')
            continue;
        if (n == cap) {
            size_t nova_cap = cap * 2;
            Acesso *cresceu = realloc(acessos, nova_cap * sizeof(*acessos));
            if (!cresceu) {
                fprintf(stderr, "Memoria insuficiente para ler o log.\n");
                free(acessos);
                fclose(arquivo);
                return NULL;
            }
            acessos = cresceu;
            cap = nova_cap;
        }
        acessos[n].pagina = addr >> s;
        acessos[n].rw = rw;
        n++;
    }

    fclose(arquivo);
    *quantidade = n;
    return acessos;
}

/* prox[i] = índice do próximo acesso à mesma página, ou NUNCA_MAIS. */
static unsigned long long *montar_proximo_uso(const Acesso *acessos, size_t n,
                                              size_t num_paginas) {
    unsigned long long *prox = malloc(n * sizeof(*prox));
    unsigned long long *ultimo = malloc(num_paginas * sizeof(*ultimo));
    if (!prox || !ultimo) {
        fprintf(stderr, "Memoria insuficiente para o algoritmo Otimo.\n");
        free(prox);
        free(ultimo);
        return NULL;
    }

    for (size_t p = 0; p < num_paginas; p++)
        ultimo[p] = NUNCA_MAIS;

    for (size_t i = n; i-- > 0;) {
        unsigned pagina = acessos[i].pagina;
        prox[i] = ultimo[pagina];
        ultimo[pagina] = (unsigned long long)i;
    }

    free(ultimo);
    return prox;
}

static int vitima_lru(const Quadro *quadros, int n) {
    int vitima = 0;
    for (int i = 1; i < n; i++) {
        if (quadros[i].ultimo_acesso < quadros[vitima].ultimo_acesso)
            vitima = i;
    }
    return vitima;
}

static int vitima_fifo(const Quadro *quadros, int n) {
    int vitima = 0;
    for (int i = 1; i < n; i++) {
        if (quadros[i].instante_carga < quadros[vitima].instante_carga)
            vitima = i;
    }
    return vitima;
}

/* Classe NRU: 0 = (R=0,M=0), 1 = (0,1), 2 = (1,0), 3 = (1,1).
 * Na mesma classe, sai a página carregada há mais tempo. */
static int classe_nru(const Quadro *quadro) {
    return (quadro->R ? 2 : 0) | (quadro->M ? 1 : 0);
}

static int vitima_nru(const Quadro *quadros, int n) {
    int vitima = 0;
    int melhor = classe_nru(&quadros[0]);
    for (int i = 1; i < n; i++) {
        int classe = classe_nru(&quadros[i]);
        if (classe < melhor ||
            (classe == melhor &&
             quadros[i].instante_carga < quadros[vitima].instante_carga)) {
            melhor = classe;
            vitima = i;
        }
    }
    return vitima;
}

static int vitima_otimo(const Quadro *quadros, int n) {
    int vitima = 0;
    for (int i = 1; i < n; i++) {
        if (quadros[i].proximo_uso > quadros[vitima].proximo_uso)
            vitima = i;
    }
    return vitima;
}

static int escolher_vitima(const Quadro *quadros, int n, Algoritmo alg) {
    switch (alg) {
    case ALG_LRU:
        return vitima_lru(quadros, n);
    case ALG_NRU:
        return vitima_nru(quadros, n);
    case ALG_FIFO:
        return vitima_fifo(quadros, n);
    case ALG_OPT:
        return vitima_otimo(quadros, n);
    }
    return 0;
}

static void zerar_bits_r(Quadro *quadros, int n) {
    for (int i = 0; i < n; i++)
        quadros[i].R = 0;
}

static void instalar(Quadro *quadros, int32_t *tabela, int quadro, unsigned pagina,
                     char rw, unsigned long long agora, unsigned long long proximo) {
    quadros[quadro].pagina = pagina;
    quadros[quadro].R = 1;
    quadros[quadro].M = (rw == 'W');
    quadros[quadro].ultimo_acesso = agora;
    quadros[quadro].instante_carga = agora;
    quadros[quadro].proximo_uso = proximo;
    tabela[pagina] = (int32_t)quadro;
}

static void simular(const Acesso *acessos, size_t n, const unsigned long long *prox,
                    int32_t *tabela, Quadro *quadros, int nquadros, Algoritmo alg,
                    unsigned long long *faltas, unsigned long long *escritas) {
    int ocupados = 0;
    unsigned long long agora = 0;

    *faltas = 0;
    *escritas = 0;

    for (size_t i = 0; i < n; i++) {
        unsigned pagina = acessos[i].pagina;
        char rw = acessos[i].rw;
        unsigned long long proximo = prox ? prox[i] : NUNCA_MAIS;
        int32_t quadro = tabela[pagina];

        if (alg == ALG_NRU && agora > 0 && agora % NRU_TICK == 0)
            zerar_bits_r(quadros, ocupados);

        if (quadro >= 0) {
            quadros[quadro].R = 1;
            if (rw == 'W')
                quadros[quadro].M = 1;
            quadros[quadro].ultimo_acesso = agora;
            quadros[quadro].proximo_uso = proximo;
        } else {
            int destino;
            if (ocupados < nquadros) {
                destino = ocupados++;
            } else {
                destino = escolher_vitima(quadros, nquadros, alg);
                if (quadros[destino].M)
                    (*escritas)++;
                tabela[quadros[destino].pagina] = -1;
            }
            instalar(quadros, tabela, destino, pagina, rw, agora, proximo);
            (*faltas)++;
        }
        agora++;
    }
}

int main(int argc, char **argv) {
    Algoritmo alg;
    int pagina_kb;
    int memoria_mb;
    int s;
    size_t bytes_pagina;
    size_t bytes_memoria;
    size_t num_paginas;
    int nquadros;
    size_t n = 0;
    Acesso *acessos = NULL;
    unsigned long long *prox = NULL;
    int32_t *tabela = NULL;
    Quadro *quadros = NULL;
    unsigned long long faltas = 0;
    unsigned long long escritas = 0;

#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (argc != 5) {
        uso();
        return 1;
    }
    if (!reconhecer_algoritmo(argv[1], &alg)) {
        fprintf(stderr, "Algoritmo invalido: %s\n", argv[1]);
        uso();
        return 1;
    }

    pagina_kb = atoi(argv[3]);
    memoria_mb = atoi(argv[4]);
    if (pagina_kb != 4 && pagina_kb != 8) {
        fprintf(stderr, "Tamanho de pagina deve ser 4 ou 8 KB.\n");
        uso();
        return 1;
    }
    if (memoria_mb != 2 && memoria_mb != 4) {
        fprintf(stderr, "Memoria fisica deve ser 2 ou 4 MB.\n");
        uso();
        return 1;
    }

    s = deslocamento_pagina(pagina_kb);
    bytes_pagina = (size_t)pagina_kb * 1024u;
    bytes_memoria = (size_t)memoria_mb * 1024u * 1024u;
    nquadros = (int)(bytes_memoria / bytes_pagina);
    num_paginas = (size_t)1u << (32 - s);

    acessos = ler_acessos(argv[2], s, &n);
    if (!acessos)
        return 1;

    if (alg == ALG_OPT && n > 0) {
        prox = montar_proximo_uso(acessos, n, num_paginas);
        if (!prox) {
            free(acessos);
            return 1;
        }
    }

    tabela = malloc(num_paginas * sizeof(*tabela));
    quadros = calloc((size_t)nquadros, sizeof(*quadros));
    if (!tabela || !quadros) {
        fprintf(stderr, "Memoria insuficiente para a tabela de paginas.\n");
        free(acessos);
        free(prox);
        free(tabela);
        free(quadros);
        return 1;
    }
    memset(tabela, 0xFF, num_paginas * sizeof(*tabela));

    printf("Executando o simulador...\n");
    fflush(stdout);

    simular(acessos, n, prox, tabela, quadros, nquadros, alg, &faltas, &escritas);

    printf("Arquivo de entrada: %s\n", argv[2]);
    printf("Tamanho da memoria fisica: %d MB\n", memoria_mb);
    printf("Tamanho das páginas: %d KB\n", pagina_kb);
    printf("Algoritmo de substituição: %s\n", nome_algoritmo(alg));
    printf("Número de Faltas de Páginas: %llu\n", faltas);
    printf("Número de Páginas Escritas: %llu\n", escritas);

    free(acessos);
    free(prox);
    free(tabela);
    free(quadros);
    return 0;
}
