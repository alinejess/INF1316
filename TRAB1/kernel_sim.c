/*
 * ============================================================================
 *  kernel_sim.c  —  KernelSim + InterControllerSim
 *  Trabalho 1 — Sistemas Operacionais (INF 1316)
 * ============================================================================
 *
 *  COMPILAR:   gcc -Wall -o app app.c
 *              gcc -Wall -o kernel_sim kernel_sim.c
 *  EXECUTAR:   ./kernel_sim
 *
 *  ----------------------------------------------------------------------------
 *  VISÃO GERAL (o que aparece na Figura 1 do enunciado)
 *  ----------------------------------------------------------------------------
 *
 *   main()  ........ é o processo KERNEL SIM.
 *                    - cria (fork) o INTERCONTROLLER SIM;
 *                    - cria (fork + exec ./app) os processos A1..An;
 *                    - fica em um laço recebendo interrupções e mensagens.
 *
 *   intercontrolador()  é o processo INTERCONTROLLER SIM.
 *                    - a cada 1 s envia IRQ0 (fim do timeslice) = SIGUSR1;
 *                    - a cada 3 s envia IRQ1 (fim de I/O em D1) = SIGUSR2.
 *
 *   app (app.c) .... são os processos de aplicação A1..An.
 *                    - executam um laço de MAX iterações;
 *                    - avisam o KernelSim pelo pipe quando fazem syscall.
 *
 *  Como o KernelSim para e continua um processo?
 *      SIGSTOP  -> congela o processo (ele para de executar)
 *      SIGCONT  -> descongela o processo (ele volta de onde parou)
 *
 *  ----------------------------------------------------------------------------
 *  ESTRUTURAS DE DADOS DO KERNEL
 *  ----------------------------------------------------------------------------
 *
 *   tabela[]      : um PCB (bloco de controle) por processo de aplicação.
 *   prontos       : fila de processos prontos para executar (ready queue).
 *   espera_io     : fila de processos bloqueados esperando o término de I/O em D1.
 *   executando    : índice do processo que está na CPU agora (-1 = ninguém).
 *
 *  ----------------------------------------------------------------------------
 *  REGRAS DE ESCALONAMENTO (resumo do enunciado)
 *  ----------------------------------------------------------------------------
 *   1. IRQ0 (fim do timeslice): o processo atual vai para o FIM da fila de
 *      prontos (SIGSTOP) e o primeiro da fila de prontos é executado (SIGCONT).
 *   2. Syscall de I/O: o processo atual é parado (SIGSTOP) e vai para a fila
 *      de espera de I/O. Nunca volta a executar antes do próximo IRQ1.
 *   3. IRQ1 (fim de I/O): o primeiro da fila de espera de I/O é desbloqueado
 *      e colocado no INÍCIO da fila de prontos (tem prioridade sobre quem
 *      terminou apenas o timeslice).
 *   4. Contexto: o PC do processo é guardado no PCB quando ele é parado,
 *      e é o que o processo continua usando quando volta.
 * ============================================================================
 */

/* Necessário para usar pselect(), sigaction() e kill() com a versão POSIX */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>      /* va_list: usado pela função de log */
#include <string.h>
#include <errno.h>
#include <signal.h>      /* sinais: kill, sigaction, SIGSTOP, SIGCONT... */
#include <unistd.h>      /* fork, exec, pipe, sleep, read, write, close */
#include <sys/types.h>
#include <sys/wait.h>    /* wait() */
#include <sys/select.h>  /* pselect() */

/* ============================================================================
 *  CONFIGURAÇÃO — mexa aqui para mudar o comportamento da simulação
 * ============================================================================ */

#define NUM_PROC   3    /* quantos processos de aplicação (A1..A3). O trabalho pede de 3 a 6 */
#define MAX_ITER   12   /* MAX: quantas iterações cada processo executa até terminar */
#define TIMESLICE  1    /* segundos: a cada TIMESLICE segundos chega uma IRQ0 */
#define IO_TIME    3    /* segundos: a cada IO_TIME segundos chega uma IRQ1 */

/* As "interrupções" são sinais Unix comuns:
     IRQ0 é representada por SIGUSR1
     IRQ1 é representada por SIGUSR2 */
#define SIG_IRQ0   SIGUSR1
#define SIG_IRQ1   SIGUSR2

/* Plano de syscalls de cada processo. Formato: "PC<tipo>", separados por vírgula.
   Ex.: "3R,7W" = no PC 3 o processo faz READ em D1; no PC 7 faz WRITE em D1. */
static const char *planos[6] = {
    "3R,7W",      /* plano do A1 */
    "4W,8R",      /* plano do A2 */
    "5R,9W",      /* plano do A3 */
    "2R,6W",      /* plano do A4 (usado só se NUM_PROC >= 4) */
    "3W,10R",     /* plano do A5 */
    "4R,11W"      /* plano do A6 */
};

/* ============================================================================
 *  TIPOS DE DADOS
 * ============================================================================ */

/* Estados possíveis de um processo de aplicação */
typedef enum {
    PRONTO,       /* pode executar, está esperando a vez na fila de prontos */
    EXECUTANDO,   /* está com a CPU agora */
    BLOQUEADO,    /* esperando o término de uma syscall de I/O em D1 */
    TERMINADO     /* já fez as MAX iterações */
} Estado;

/* PCB — Process Control Block: tudo que o KernelSim guarda sobre um processo */
typedef struct {
    int         id;          /* número do processo: 1 = A1, 2 = A2, ... */
    pid_t       pid;         /* pid Unix do processo filho */
    Estado      estado;      /* um dos quatro estados acima */
    int         pc;          /* CONTEXTO: último PC informado pelo processo */
    int         sc_pc;       /* CONTEXTO da syscall: em qual PC ela ocorreu */
    char        sc_tipo;     /* CONTEXTO da syscall: 'R' ou 'W' */
    const char *plano;       /* plano de syscalls deste processo */
} PCB;

/* Fila circular simples de índices (índices são posições dentro de tabela[]).
   Cada processo aparece em no máximo UMA fila por vez, então NUM_PROC basta. */
typedef struct {
    int itens[NUM_PROC];
    int inicio;   /* posição do primeiro elemento da fila */
    int n;        /* quantos elementos estão na fila agora */
} Fila;

/* ============================================================================
 *  VARIÁVEIS GLOBAIS
 * ============================================================================ */

static PCB   tabela[NUM_PROC];   /* os PCBs de A1..An */
static Fila  prontos;            /* fila de prontos (ready queue) */
static Fila  espera_io;          /* fila de espera de I/O em D1 */
static int   executando = -1;    /* índice do processo na CPU; -1 = CPU ociosa */
static int   terminados = 0;     /* quantos processos já terminaram */
static int   tempo = 0;          /* relógio simulado, em segundos (para os logs) */
static int   pipe_leitura;       /* extremidade de leitura do pipe (KernelSim lê daqui) */
static pid_t kernel_pid;         /* pid do próprio KernelSim (o InterControllerSim precisa dele) */

/* Flags ligadas pelos handlers de sinal.
   "volatile sig_atomic_t" é o tipo correto para variáveis alteradas por sinais. */
static volatile sig_atomic_t flag_irq0 = 0;
static volatile sig_atomic_t flag_irq1 = 0;

/* ============================================================================
 *  FUNÇÕES DE LOG — para a linha do tempo pedida no relatório
 * ============================================================================ */

/* Imprime uma linha com o tempo simulado na frente. Uso igual ao printf. */
static void log_evento(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[t=%3ds] ", tempo);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);   /* garante que a linha aparece na hora */
    va_end(ap);
}

/* ============================================================================
 *  FUNÇÕES DAS FILAS
 * ============================================================================ */

/* Coloca 'x' no FIM da fila (usado no fim de timeslice e no bloqueio de I/O) */
static void fila_push_fim(Fila *f, int x)
{
    f->itens[(f->inicio + f->n) % NUM_PROC] = x;
    f->n++;
}

/* Coloca 'x' no INÍCIO da fila (usado quando o I/O termina: prioridade máxima) */
static void fila_push_inicio(Fila *f, int x)
{
    f->inicio = (f->inicio + NUM_PROC - 1) % NUM_PROC;  /* anda uma posição para trás */
    f->itens[f->inicio] = x;
    f->n++;
}

/* Retira e devolve o primeiro elemento. Devolve -1 se a fila está vazia. */
static int fila_pop(Fila *f)
{
    if (f->n == 0)
        return -1;
    int x = f->itens[f->inicio];
    f->inicio = (f->inicio + 1) % NUM_PROC;
    f->n--;
    return x;
}

/* Remove 'x' do meio da fila (usado quando um processo muda de fila "por fora").
   Devolve 1 se achou e removeu, 0 caso contrário. */
static int fila_remove(Fila *f, int x)
{
    for (int k = 0; k < f->n; k++) {
        int pos = (f->inicio + k) % NUM_PROC;
        if (f->itens[pos] == x) {
            /* puxa todos os elementos seguintes uma posição para trás */
            for (int j = k; j < f->n - 1; j++) {
                f->itens[(f->inicio + j) % NUM_PROC] =
                    f->itens[(f->inicio + j + 1) % NUM_PROC];
            }
            f->n--;
            return 1;
        }
    }
    return 0;
}

/* ============================================================================
 *  CONTROLE DOS PROCESSOS (SIGSTOP / SIGCONT)
 * ============================================================================ */

/* "Para" o processo i: o SO congela ele inteiramente */
static void suspende(int i)
{
    kill(tabela[i].pid, SIGSTOP);
}

/* "Continua" o processo i: ele volta exatamente de onde parou */
static void retoma(int i)
{
    kill(tabela[i].pid, SIGCONT);
}

/* ============================================================================
 *  ESCALONADOR
 * ============================================================================ */

/* Se a CPU está livre, pega o primeiro da fila de prontos e o põe para executar.
   Chamada sempre que algo muda (fim de timeslice, bloqueio, término...). */
static void escalona(void)
{
    if (executando != -1)
        return;   /* a CPU já está ocupada, nada a fazer */

    int prox = fila_pop(&prontos);
    if (prox == -1) {
        log_evento("CPU ociosa: nenhum processo pronto");
        return;
    }

    executando = prox;
    tabela[prox].estado = EXECUTANDO;

    /* A "restauração do contexto" acontece aqui: o processo volta a executar
       com o PC que tinha quando foi parado (guardado em tabela[prox].pc).
       Como o SO congela o processo inteiro no SIGSTOP, o PC que ele estava
       usando continua lá; o KernelSim mantém a cópia no PCB. */
    retoma(prox);

    log_evento("ESCALONADO A%d (retomado com PC=%d)", tabela[prox].id, tabela[prox].pc);
}

/* ============================================================================
 *  TRATAMENTO DE INTERRUPÇÕES E EVENTOS
 * ============================================================================ */

/* IRQ0 — fim do timeslice do processo que está executando */
static void trata_irq0(void)
{
    tempo += TIMESLICE;
    log_evento("IRQ0: fim do timeslice");

    if (executando != -1) {
        int i = executando;

        suspende(i);                        /* 1) para o processo (SIGSTOP)      */
        tabela[i].estado = PRONTO;          /* 2) ele volta a ficar pronto        */
        fila_push_fim(&prontos, i);         /* 3) vai para o FIM da fila de prontos */
        log_evento("A%d parado (timeslice esgotado, PC=%d) -> fim da fila de prontos",
                   tabela[i].id, tabela[i].pc);
        executando = -1;
    }

    escalona();   /* escolhe o próximo da fila de prontos */
}

/* IRQ1 — o dispositivo D1 terminou uma operação de I/O */
static void trata_irq1(void)
{
    log_evento("IRQ1: I/O em D1 terminou");

    /* Tira da fila de espera o processo que estava há mais tempo esperando */
    int i = fila_pop(&espera_io);
    if (i == -1) {
        log_evento("IRQ1 ignorado: ninguém estava esperando D1");
        return;
    }

    tabela[i].estado = PRONTO;
    /* Processos que terminaram I/O têm PRIORIDADE: entram no INÍCIO da fila de prontos */
    fila_push_inicio(&prontos, i);
    log_evento("A%d desbloqueado -> início da fila de prontos", tabela[i].id);

    escalona();   /* se a CPU estiver livre, ele já pode executar */
}

/* Mensagem "SYSCALL": o processo i pediu I/O em D1 no PC informado */
static void trata_syscall(int i, int pc, char tipo)
{
    PCB *p = &tabela[i];

    if (p->estado == TERMINADO)
        return;

    /* Guarda o CONTEXTO da syscall: PC e tipo (R/W). Isso faz parte do
       contexto a ser salvo, conforme o enunciado. */
    p->pc      = pc;
    p->sc_pc   = pc;
    p->sc_tipo = tipo;

    /* Pode acontecer de o processo ter sido parado pelo timeslice antes de
       a mensagem chegar. Nesse caso ele está na fila de prontos: removemos. */
    if (p->estado == EXECUTANDO)
        executando = -1;
    else if (p->estado == PRONTO)
        fila_remove(&prontos, i);

    suspende(i);                        /* 1) para o processo (SIGSTOP)        */
    p->estado = BLOQUEADO;              /* 2) ele fica bloqueado               */
    fila_push_fim(&espera_io, i);       /* 3) entra na fila de espera de I/O   */

    log_evento("A%d fez syscall D1 %c no PC=%d -> BLOQUEADO (fila de espera de I/O)",
               p->id, tipo, pc);

    escalona();
}

/* Mensagem "DONE": o processo i completou MAX iterações */
static void trata_fim(int i)
{
    PCB *p = &tabela[i];

    if (p->estado == EXECUTANDO)
        executando = -1;
    else if (p->estado == PRONTO)
        fila_remove(&prontos, i);

    p->estado = TERMINADO;
    terminados++;
    log_evento("A%d TERMINOU (PC final=%d)", p->id, p->pc);

    escalona();
}

/* Mensagem "PC": guarda o PC atual do processo (parte do contexto) */
static void trata_pc(int i, int pc)
{
    tabela[i].pc = pc;
}

/* Lê as linhas que os processos de aplicação escreveram no pipe e as interpreta.
   Devolve 0 se o pipe foi fechado (EOF), 1 caso contrário. */
static int ler_mensagens(void)
{
    char buf[1024];
    ssize_t n = read(pipe_leitura, buf, sizeof buf - 1);
    if (n <= 0)
        return 0;

    buf[n] = '\0';

    /* Uma leitura pode trazer várias mensagens, uma por linha */
    char *linha = strtok(buf, "\n");
    while (linha != NULL) {
        int id, pc;
        char tipo;

        if (sscanf(linha, "PC %d %d", &id, &pc) == 2) {
            trata_pc(id - 1, pc);
        } else if (sscanf(linha, "SYSCALL %d %d %c", &id, &pc, &tipo) == 3) {
            trata_syscall(id - 1, pc, tipo);
        } else if (sscanf(linha, "DONE %d", &id) == 1) {
            trata_fim(id - 1);
        }
        linha = strtok(NULL, "\n");
    }
    return 1;
}

/* ============================================================================
 *  HANDLERS DE SINAL
 *  Devem ser curtos: só marcam a flag. O trabalho pesado é feito no loop.
 * ============================================================================ */

static void handler_irq0(int sig)
{
    (void)sig;
    flag_irq0 = 1;
}

static void handler_irq1(int sig)
{
    (void)sig;
    flag_irq1 = 1;
}

/* ============================================================================
 *  INTERCONTROLLER SIM — processo separado que gera as interrupções
 * ============================================================================ */

static void intercontrolador(pid_t alvo)
{
    int segundos = 0;

    while (1) {
        sleep(TIMESLICE);                /* espera 1 s (conforme o enunciado) */
        segundos += TIMESLICE;

        kill(alvo, SIG_IRQ0);            /* IRQ0 a cada 1 s                */

        if (segundos % IO_TIME == 0)
            kill(alvo, SIG_IRQ1);        /* IRQ1 a cada 3 s                */
    }
}

/* ============================================================================
 *  MAIN — é o processo KERNEL SIM
 * ============================================================================ */

int main(void)
{
    int fd[2];                 /* fd[0] = leitura, fd[1] = escrita do pipe */
    sigset_t bloqueio;         /* conjunto com IRQ0 e IRQ1 */
    sigset_t antiga;           /* máscara de sinais original (antes de bloquear) */
    sigset_t vazia;            /* máscara vazia: usada no pselect */
    struct sigaction sa;

    /* ---------- 1. Instalar os handlers das interrupções ---------- */
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;           /* sem SA_RESTART: queremos que pselect retorne EINTR */

    sa.sa_handler = handler_irq0;
    sigaction(SIG_IRQ0, &sa, NULL);

    sa.sa_handler = handler_irq1;
    sigaction(SIG_IRQ1, &sa, NULL);

    /* ---------- 2. Bloquear IRQ0 e IRQ1 ----------
       Motivo: se um sinal chegar entre o "if (flag)" e o "pselect()", ele se
       perderia. Bloqueando e usando pselect(), o sinal só é entregue
       durante a espera, sem perda. */
    sigemptyset(&bloqueio);
    sigaddset(&bloqueio, SIG_IRQ0);
    sigaddset(&bloqueio, SIG_IRQ1);
    sigprocmask(SIG_BLOCK, &bloqueio, &antiga);
    sigemptyset(&vazia);

    /* ---------- 3. Criar o pipe de comunicação aplicações -> KernelSim ---------- */
    if (pipe(fd) == -1) {
        perror("pipe");
        exit(1);
    }
    pipe_leitura = fd[0];
    kernel_pid   = getpid();

    /* ---------- 4. Criar o processo INTERCONTROLLER SIM ---------- */
    pid_t irq_pid = fork();
    if (irq_pid == 0) {
        /* Filho: restaura a máscara original (os sinais devem chegar ao KernelSim,
           não ao filho) e fecha o pipe, que não é dele. */
        sigprocmask(SIG_SETMASK, &antiga, NULL);
        close(fd[0]);
        close(fd[1]);
        intercontrolador(kernel_pid);
        _exit(0);
    }

    /* ---------- 5. Criar os processos de aplicação A1..An ---------- */
    for (int i = 0; i < NUM_PROC; i++) {
        /* Preenche o PCB antes de criar o processo */
        tabela[i].id         = i + 1;
        tabela[i].estado     = PRONTO;
        tabela[i].pc         = 0;
        tabela[i].sc_pc      = 0;
        tabela[i].sc_tipo    = '-';
        tabela[i].plano      = planos[i];

        pid_t pid = fork();
        if (pid == 0) {
            /* Filho: vira o programa ./app. Antes, restaura a máscara de sinais. */
            sigprocmask(SIG_SETMASK, &antiga, NULL);
            close(fd[0]);   /* o processo de aplicação só escreve no pipe */

            char a_id[16], a_max[16], a_fd[16];
            snprintf(a_id,  sizeof a_id,  "%d", i + 1);
            snprintf(a_max, sizeof a_max, "%d", MAX_ITER);
            snprintf(a_fd,  sizeof a_fd,  "%d", fd[1]);

            /* execl substitui o código do filho pelo do programa ./app */
            execl("./app", "app", a_id, a_max, a_fd, planos[i], (char *)NULL);

            /* Só chegamos aqui se o execl falhar */
            perror("execl");
            _exit(1);
        }

        /* Pai (KernelSim): guarda o pid e deixa o processo PARADO no início.
           Assim, só o escalonador decide quem começa. */
        tabela[i].pid = pid;
        suspende(i);
        fila_push_fim(&prontos, i);
    }

    /* O KernelSim não escreve no pipe: fecha a ponta de escrita. Assim, quando
       todos os processos de aplicação terminarem, o read() devolve EOF. */
    close(fd[1]);

    log_evento("KernelSim iniciado: %d processos, TIMESLICE=%ds, IO=%ds, MAX=%d",
               NUM_PROC, TIMESLICE, IO_TIME, MAX_ITER);

    /* Primeira escolha: o primeiro da fila de prontos começa a executar */
    escalona();

    /* ---------- 6. Laço principal do KernelSim ---------- */
    while (terminados < NUM_PROC) {

        /* pselect espera ao mesmo tempo por:
             - dados no pipe (mensagens dos processos), e
             - um sinal de IRQ0 ou IRQ1 (entregue enquanto estamos esperando).
           Durante a espera a máscara é "vazia", então os sinais bloqueados
           podem chegar. Ao retornar, a máscara volta ao normal. */
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipe_leitura, &rfds);

        int r = pselect(pipe_leitura + 1, &rfds, NULL, NULL, NULL, &vazia);
        if (r == -1 && errno != EINTR) {
            perror("pselect");
            break;
        }

        /* Trata as interrupções que chegaram (se alguma) */
        if (flag_irq0) {
            flag_irq0 = 0;
            trata_irq0();
        }
        if (flag_irq1) {
            flag_irq1 = 0;
            trata_irq1();
        }

        /* Trata as mensagens dos processos (se houver) */
        if (r > 0 && FD_ISSET(pipe_leitura, &rfds)) {
            if (!ler_mensagens()) {
                log_evento("pipe fechado inesperadamente");
                break;
            }
        }
    }

    /* ---------- 7. Encerramento ---------- */
    kill(irq_pid, SIGKILL);            /* para o InterControllerSim */
    while (wait(NULL) > 0)              /* recolhe todos os filhos (evita zumbis) */
        ;

    /* Resumo final: estado e PC de cada processo */
    printf("\n===== RESUMO FINAL =====\n");
    for (int i = 0; i < NUM_PROC; i++) {
        printf("A%d: PC final = %d, estado = %s\n",
               tabela[i].id, tabela[i].pc,
               tabela[i].estado == TERMINADO ? "TERMINADO" : "NÃO TERMINOU");
    }

    return 0;
}