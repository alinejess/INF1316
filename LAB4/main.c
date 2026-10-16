// Aline Jéssica David Gonçalves
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#define NUM_TAREFAS 6
#define QUANTUM 1          // quantum em segundos
#define MAX_SLICES 5       // numero de time-slices permitido

// Codigo executado pela TAREFA (processo filho)
void executarTarefa(int id) {
    int contador = 0;

    while (1) {
        contador++;
        printf("   [Tarefa %d - pid %d] executando... (passo %d)\n",
               id, getpid(), contador);
        fflush(stdout);
        usleep(200000); /* 0.2s, so para nao poluir demais a tela */
    }
}

// Codigo executado pelo PAI (escalonador)
int main(void) {
    pid_t pids[NUM_TAREFAS];
    int slices[NUM_TAREFAS];   // quantos time-slices cada tarefa ja recebeu
    int ativo[NUM_TAREFAS];    // 1 = ainda esta na fila, 0 = ja foi morta
    int i;
    int tarefasRestantes = NUM_TAREFAS;

    /* Deixa a saida sem buffer: evita que mensagens ja impressas pelo pai
     * sejam "duplicadas" quando o fork() copia o buffer do stdout. */
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("Escalonador: criando %d tarefas...\n", NUM_TAREFAS);

    // Cria as 6 tarefas (fork)
    for (i = 0; i < NUM_TAREFAS; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            fprintf(stderr, "Erro ao criar tarefa %d\n", i);
            exit(1);
        }

        if (pid == 0) {
            // processo filho: executa a tarefa e nunca volta daqui
            executarTarefa(i);
            exit(0); // nunca deve chegar aqui, pois o laco e eterno
        }

        /* processo pai: guarda o pid e ja para o filho, pois ele
         * so deve rodar quando for a vez dele no round-robin */
        pids[i] = pid;
        slices[i] = 0;
        ativo[i] = 1;

        kill(pids[i], SIGSTOP);
        printf("Escalonador: tarefa %d criada (pid %d) e colocada em espera\n",
               i, pids[i]);
    }

    printf("Escalonador: iniciando round-robin (quantum = %d segundo(s))\n\n",
           QUANTUM);

    // Laco do escalonador: roda enquanto existir alguma tarefa ativa
    while (tarefasRestantes > 0) {
        for (i = 0; i < NUM_TAREFAS; i++) {
            if (!ativo[i])
                continue; // tarefa ja foi encerrada, pula

            // inicio do quantum da tarefa i
            printf("Escalonador: --> CONT tarefa %d (pid %d) "
                   "[time-slice %d/%d]\n",
                   i, pids[i], slices[i] + 1, MAX_SLICES);

            kill(pids[i], SIGCONT);
            sleep(QUANTUM);          // deixa a tarefa rodar por 1 segundo
            kill(pids[i], SIGSTOP);  // preempcao 

            slices[i]++;

            printf("Escalonador: <-- STOP tarefa %d (pid %d) "
                   "[completou %d/%d time-slices]\n",
                   i, pids[i], slices[i], MAX_SLICES);

            // verifica se a tarefa ja cumpriu o limite
            if (slices[i] >= MAX_SLICES) {
                printf("Escalonador: tarefa %d (pid %d) atingiu o limite de "
                       "%d time-slices. Encerrando com SIGKILL.\n",
                       i, pids[i], MAX_SLICES);

                kill(pids[i], SIGKILL);
                waitpid(pids[i], NULL, 0); // remove o processo zumbi

                ativo[i] = 0;
                tarefasRestantes--;

                printf("Escalonador: tarefa %d finalizada. "
                       "Tarefas restantes: %d\n", i, tarefasRestantes);
            }

            printf("\n");
        }
    }

    printf("Escalonador: todas as tarefas foram concluidas. Fim da execucao.\n");

    return 0;
}