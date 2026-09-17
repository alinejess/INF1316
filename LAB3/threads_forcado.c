#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
 
#define TAM 10000
 
int vet[TAM];
atomic_int start_flag = 0;
 
void *trabalhador(void *arg) {
    (void) arg;
    while (atomic_load(&start_flag) == 0) {}
    for (int i = 0; i < TAM; i++) {
        int tmp = vet[i];      // LE
        if ((i % 50) == 0) sched_yield();   // forca troca de contexto no meio
        tmp = tmp * 2 + 2;     // CALCULA
        vet[i] = tmp;          // ESCREVE (read-modify-write NAO atomico)
    }
    return NULL;
}
 
int main(int argc, char *argv[]) {
    int nthreads = 8;
    if (argc > 1) nthreads = atoi(argv[1]);
 
    for (int i = 0; i < TAM; i++) vet[i] = 4;
 
    pthread_t *tid = malloc(nthreads * sizeof(pthread_t));
    for (int t = 0; t < nthreads; t++) pthread_create(&tid[t], NULL, trabalhador, NULL);
    atomic_store(&start_flag, 1);
    for (int t = 0; t < nthreads; t++) pthread_join(tid[t], NULL);
 
    int iguais = 1;
    for (int i = 1; i < TAM; i++) if (vet[i] != vet[0]) { iguais = 0; break; }
    printf("== THREADS COM ENTRELACAMENTO FORCADO | nthreads=%d ==\n", nthreads);
    printf("Todas as posicoes com o mesmo valor? %s\n", iguais ? "SIM" : "NAO");
    printf("Exemplos: vet[0]=%d vet[1]=%d vet[50]=%d vet[100]=%d vet[9999]=%d\n",
           vet[0], vet[1], vet[50], vet[100], vet[TAM-1]);
 
    int distintos = 0;
    for (int i = 0; i < TAM && distintos <= 20; i++) {
        int achou = 0;
        for (int j = 0; j < i; j++) if (vet[j] == vet[i]) { achou = 1; break; }
        if (!achou) distintos++;
    }
    printf("Valores distintos encontrados (limitado a 20): %d (esperado sem corrida: 1, valendo 1534)\n", distintos);
 
    free(tid);
    return 0;
}
 