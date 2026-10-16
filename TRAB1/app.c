/*
 * ============================================================================
 *  app.c  —  Processo de aplicação (A1, A2, A3, ...)
 * ============================================================================
 *
 *  Este programa NÃO é executado diretamente pelo usuário. Ele é lançado pelo
 *  KernelSim com fork() + execl(). Os argumentos são:
 *
 *      ./app  <id>  <MAX>  <fd_escrita>  <plano>
 *
 *      <id>            número do processo (1 para A1, 2 para A2, ...)
 *      <MAX>           quantas iterações o laço deve executar
 *      <fd_escrita>    descritor do pipe para mandar mensagens ao KernelSim
 *      <plano>         lista das syscalls, ex.: "3R,7W"
 *                      significa: no PC 3 faz read em D1; no PC 7 faz write em D1
 *
 *  O processo NÃO sabe que está em time-sharing: ele só executa seu laço.
 *  Quem decide quando ele para (SIGSTOP) ou continua (SIGCONT) é o KernelSim.
 *
 *  Mensagens enviadas pelo pipe (uma por linha):
 *      "PC <id> <pc>\n"              -> a cada iteração, informa o PC atual
 *      "SYSCALL <id> <pc> <R|W>\n"   -> ao chegar no PC de uma syscall
 *      "DONE <id>\n"                 -> ao terminar as MAX iterações
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_SYSCALLS 8   /* quantas syscalls, no máximo, cabem no plano */

/* Uma syscall planejada: em qual PC ocorre e se é leitura (R) ou escrita (W) */
typedef struct {
    int  pc;     /* PC em que a syscall acontece */
    char tipo;   /* 'R' = read, 'W' = write  */
} Syscall;

/* Envia uma string pelo pipe para o KernelSim.
   Mensagens curtas (< PIPE_BUF bytes) são escritas de forma atômica pelo SO. */
static void enviar(int fd, const char *msg)
{
    write(fd, msg, strlen(msg));
}

int main(int argc, char *argv[])
{
    /* ---------- 1. Ler os argumentos passados pelo KernelSim ---------- */
    if (argc != 5) {
        fprintf(stderr, "uso: %s <id> <MAX> <fd> <plano>\n", argv[0]);
        return 1;
    }

    int  id  = atoi(argv[1]);   /* número do processo: 1, 2, 3... */
    int  max = atoi(argv[2]);   /* MAX: total de iterações do laço */
    int  fd  = atoi(argv[3]);   /* descritor do pipe de escrita */

    /* strtok() altera a string original, então copiamos o plano antes */
    char plano[256];
    strncpy(plano, argv[4], sizeof plano - 1);
    plano[sizeof plano - 1] = '\0';

    /* ---------- 2. Transformar o texto do plano em um vetor de structs ---------- */
    /* Exemplo: "3R,7W" vira [ {pc=3, tipo='R'}, {pc=7, tipo='W'} ] */
    Syscall sc[MAX_SYSCALLS];
    int nsc = 0;

    char *token = strtok(plano, ",");
    while (token != NULL && nsc < MAX_SYSCALLS) {
        sc[nsc].pc   = atoi(token);                 /* "3R" -> 3 (atoi para na letra) */
        sc[nsc].tipo = token[strlen(token) - 1];    /* último caractere: 'R' ou 'W' */
        nsc++;
        token = strtok(NULL, ",");
    }

    /* ---------- 3. O laço principal ---------- */
    char msg[128];

    /* PC começa em 1 e vai até MAX. Cada iteração = 1 "unidade de trabalho". */
    for (int pc = 1; pc <= max; pc++) {

        /* sleep(1) simula 1 segundo de trabalho da CPU.
           Se o KernelSim mandar SIGSTOP enquanto dormimos, o processo congela aqui
           e só volta quando receber SIGCONT. */
        sleep(1);

        /* Informa ao KernelSim em qual PC estamos (isto é o "contexto" do processo) */
        snprintf(msg, sizeof msg, "PC %d %d\n", id, pc);
        enviar(fd, msg);

        /* Se o PC atual é o de alguma syscall planejada, avisa o KernelSim.
           O KernelSim, ao receber, manda SIGSTOP neste processo e o bloqueia. */
        for (int k = 0; k < nsc; k++) {
            if (sc[k].pc == pc) {
                snprintf(msg, sizeof msg, "SYSCALL %d %d %c\n", id, pc, sc[k].tipo);
                enviar(fd, msg);
            }
        }
    }

    /* ---------- 4. Terminou as MAX iterações ---------- */
    snprintf(msg, sizeof msg, "DONE %d\n", id);
    enviar(fd, msg);

    close(fd);
    return 0;
}