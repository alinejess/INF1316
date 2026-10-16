// Aline Jéssica David Gonçalves, 2320276
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>

#define TAM        10000
#define VALOR_INI  10
#define NBLOCOS    100 // 100 blocos de 100 posicoes
#define TAM_BLOCO  (TAM / NBLOCOS)

int   vetor[TAM]; // vetor na memoria global 
sem_t sem_bloco[NBLOCOS]; // um semaforo por bloco

void *tarefa(void *arg) {
    long k = (long)arg;
    int delta = (k % 2 == 0) ? (int)k : -(int)k;

    for (int b = 0; b < NBLOCOS; b++) {
        // Down: entra na regiao critica
        sem_wait(&sem_bloco[b]);
        for (int i = b * TAM_BLOCO; i < (b + 1) * TAM_BLOCO; i++)
            vetor[i] = vetor[i] + delta;
        // Up: sai da regiao critica
        sem_post(&sem_bloco[b]);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int n = (argc > 1) ? atoi(argv[1]) : 100;
    pthread_t *th = malloc(n * sizeof(pthread_t));
    if (th == NULL) { perror("malloc"); return 1; }

    for (int i = 0; i < TAM; i++)
        vetor[i] = VALOR_INI;

    // sem_init(semaforo, 0 = compartilhado entre threads do processo, valor inicial 1)
    for (int b = 0; b < NBLOCOS; b++)
        if (sem_init(&sem_bloco[b], 0, 1) != 0) { perror("sem_init"); return 1; }

    long esperado = VALOR_INI;
    for (long k = 1; k <= n; k++)
        esperado += (k % 2 == 0) ? k : -k;

    for (long k = 1; k <= n; k++)
        if (pthread_create(&th[k - 1], NULL, tarefa, (void *)k) != 0) {
            perror("pthread_create"); return 1;
        }

    for (int k = 0; k < n; k++)
        pthread_join(th[k], NULL);

    int erros = 0;
    printf("Tarefas: %d | valor esperado em todas as posicoes: %ld\n", n, esperado);
    for (int i = 0; i < TAM; i++) {
        if (vetor[i] != esperado) {
            if (erros < 50)
                printf("  ERRO na posicao %5d: valor = %d (diferenca %+ld)\n",
                       i, vetor[i], vetor[i] - esperado);
            erros++;
        }
    }
    if (erros == 0)
        printf("Nenhum erro: todas as %d posicoes valem %ld.\n", TAM, esperado);
    else
        printf("Posicoes com erro de concorrencia: %d de %d\n", erros, TAM);

    for (int b = 0; b < NBLOCOS; b++)
        // equivalente a sem_close/sem_unlink p/ sem_init
        sem_destroy(&sem_bloco[b]);

    free(th);
    return 0;
}