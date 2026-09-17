// aline Jéssica David Gonçalves - 2320276
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <stdatomic.h>
 
#define TAM 10000
 
int vet[TAM];
atomic_int start_flag = 0;
 
static double agora(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
 
void *trabalhador(void *arg) {
    (void) arg;
    while (atomic_load(&start_flag) == 0) { } // busy wait
    for (int i = 0; i < TAM; i++) {
        vet[i] = vet[i] * 2 + 2;   // SEM nenhuma exclusão mútua
    }
    return NULL;
}
 
int main(int argc, char *argv[]) {
    int nthreads = 8;
    if (argc > 1) nthreads = atoi(argv[1]);
 
    for (int i = 0; i < TAM; i++) vet[i] = 4;
    atomic_store(&start_flag, 0);
 
    pthread_t *tid = malloc(nthreads * sizeof(pthread_t));
 
    for (int t = 0; t < nthreads; t++) {
        if (pthread_create(&tid[t], NULL, trabalhador, NULL) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }
 
    // todas as threads ja foram criadas -> criacao terminou
    double t0 = agora();
    atomic_store(&start_flag, 1);
 
    for (int t = 0; t < nthreads; t++) pthread_join(tid[t], NULL);
    double t1 = agora();
 
    printf("== THREADS (pthreads) | nthreads=%d ==\n", nthreads);
    printf("Tempo de execucao (so o trabalho): %.6f s\n", t1 - t0);
 
    int iguais = 1;
    for (int i = 1; i < TAM; i++) {
        if (vet[i] != vet[0]) { iguais = 0; break; }
    }
    printf("Todas as posicoes com o mesmo valor? %s\n", iguais ? "SIM" : "NAO");
 
    int distintos = 0;
    for (int i = 0; i < TAM; i++) {
        int achou = 0;
        for (int j = 0; j < i; j++) if (vet[j] == vet[i]) { achou = 1; break; }
        if (!achou) distintos++;
        if (distintos > 50) break;
    }
    printf("Valor esperado SEM perda de atualizacoes (8 aplicacoes seguidas de f(x)=2x+2 a partir de 4): 1534\n");
    printf("Exemplos de valores obtidos: vet[0]=%d vet[1]=%d vet[100]=%d vet[9999]=%d\n",
           vet[0], vet[1], vet[100], vet[TAM - 1]);
    printf("Quantidade de valores distintos encontrados (limitado a 50): %d\n", distintos);
 
    free(tid);
    return 0;
}
 