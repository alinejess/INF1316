// Aline Jéssica David Gonçalves - 2320276
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>

#define TAM 10000

static double agora(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char *argv[]) {
    int nproc = 8;
    if (argc > 1) nproc = atoi(argv[1]);

    // vetor compartilhado entre pai e filhos
    int *vet = mmap(NULL, TAM * sizeof(int), PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    // flag de sincronismo, também compartilhada
    atomic_int *start_flag = mmap(NULL, sizeof(atomic_int), PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    for (int i = 0; i < TAM; i++) vet[i] = 4;
    atomic_store(start_flag, 0);

    pid_t *pids = malloc(nproc * sizeof(pid_t));

    for (int p = 0; p < nproc; p++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); exit(1); }
        if (pid == 0) {
            // filho: espera o sinal do pai para só então começar a contar 
            while (atomic_load(start_flag) == 0) { } // busy wait
            for (int i = 0; i < TAM; i++) {
                vet[i] = vet[i] * 2 + 2;   // SEM nenhuma exclusão mútua
            }
            _exit(0);
        } else {
            pids[p] = pid;
        }
    }

    // nesse ponto todos os fork() já retornaram no pai -> criação terminou
    double t0 = agora();
    atomic_store(start_flag, 1);   // dispara todo mundo junto

    for (int p = 0; p < nproc; p++) waitpid(pids[p], NULL, 0);
    double t1 = agora();

    printf("== PROCESSOS (fork) | nproc=%d ==\n", nproc);
    printf("Tempo de execucao (so o trabalho): %.6f s\n", t1 - t0);

    int iguais = 1;
    for (int i = 1; i < TAM; i++) {
        if (vet[i] != vet[0]) { iguais = 0; break; }
    }
    printf("Todas as posicoes com o mesmo valor? %s\n", iguais ? "SIM" : "NAO");

    // conta quantos valores diferentes existem no vetor final 
    // (varredura simples, ok para fins didaticos)
    int distintos = 0;
    for (int i = 0; i < TAM; i++) {
        int achou = 0;
        for (int j = 0; j < i; j++) if (vet[j] == vet[i]) { achou = 1; break; }
        if (!achou) distintos++;
        if (distintos > 50) break; // nao precisa contar tudo, so mostrar que tem varios
    }
    printf("Valor esperado SEM perda de atualizacoes (8 aplicacoes seguidas de f(x)=2x+2 a partir de 4): 1534\n");
    printf("Exemplos de valores obtidos: vet[0]=%d vet[1]=%d vet[100]=%d vet[9999]=%d\n",
           vet[0], vet[1], vet[100], vet[TAM - 1]);
    printf("Quantidade de valores distintos encontrados (limitado a 50): %d\n", distintos);

    free(pids);
    return 0;
}