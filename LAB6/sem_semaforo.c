// Aline Jéssica David Gonçalves, 2320276
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define TAM        10000
#define VALOR_INI  10

int vetor[TAM]; // vetor na memoria global (compartilhado)

void *tarefa(void *arg) {
    long k = (long)arg; // numero da tarefa: 1..N
    int delta = (k % 2 == 0) ? (int)k : -(int)k;

    for (int i = 0; i < TAM; i++)
        // le - calcula - escreve: NAO atomico
        vetor[i] = vetor[i] + delta;

    return NULL;
}

int main(int argc, char *argv[]) {
    int n = (argc > 1) ? atoi(argv[1]) : 100;
    pthread_t *th = malloc(n * sizeof(pthread_t));
    if (th == NULL) { perror("malloc"); return 1; }

    for (int i = 0; i < TAM; i++)
        vetor[i] = VALOR_INI;

    // valor esperado: 10 + (-1 +2 -3 +4 ... )
    long esperado = VALOR_INI;
    for (long k = 1; k <= n; k++)
        esperado += (k % 2 == 0) ? k : -k;

    for (long k = 1; k <= n; k++)
        if (pthread_create(&th[k - 1], NULL, tarefa, (void *)k) != 0) {
            perror("pthread_create"); return 1;
        }

    for (int k = 0; k < n; k++)
        pthread_join(th[k], NULL);

    // verificacao: compara cada posicao com o valor esperado
    // posicoes consecutivas com o mesmo valor errado sao agrupadas em faixas
    int erros = 0, faixas = 0;
    printf("Tarefas: %d | valor esperado em todas as posicoes: %ld\n", n, esperado);
    for (int i = 0; i < TAM; ) {
        if (vetor[i] == esperado) { i++; continue; }
        int ini = i;
        while (i < TAM && vetor[i] == vetor[ini]) i++;
        erros += i - ini;
        if (faixas++ < 40)
            printf("  ERRO nas posicoes %5d a %5d (%4d pos.): valor = %d (diferenca %+ld)\n",
                   ini, i - 1, i - ini, vetor[ini], vetor[ini] - esperado);
    }
    if (faixas > 40)
        printf("  ... (%d faixas com erro; so as 40 primeiras foram listadas)\n", faixas);

    if (erros == 0)
        printf("Nenhum erro de concorrencia detectado nesta execucao.\n");
    else
        printf("Posicoes com erro de concorrencia: %d de %d\n", erros, TAM);

    free(th);
    return 0;
}