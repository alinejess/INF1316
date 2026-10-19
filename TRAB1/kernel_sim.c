// Aline Jéssica David Gonçalves, 2320276
/*
 * kernel_sim.c — KernelSim + InterControllerSim (INF1316, Trabalho 1)
 *
 * Compilar:  gcc -Wall -Wextra -o app app.c
 *            gcc -Wall -Wextra -o kernel_sim kernel_sim.c
 * Executar:  ./kernel_sim      (o ./app precisa estar na mesma pasta)
 *
 * Processo KernelSim (este programa): cria os processos de aplicação e o
 * InterControllerSim, recebe as mensagens pelo pipe e decide quem usa a CPU.
 */

#define _POSIX_C_SOURCE 200809L   /* habilita pselect, sigaction, kill etc. */

#include <stdio.h>        /* printf, snprintf, sscanf */
#include <stdlib.h>       /* exit, atoi */
#include <stdarg.h>       /* va_list: função de log com argumentos variáveis */
#include <string.h>       /* memset, strtok */
#include <errno.h>        /* errno, EINTR */
#include <signal.h>       /* sigaction, kill, SIGSTOP, SIGCONT, SIGUSR1/2 */
#include <unistd.h>       /* fork, execl, pipe, sleep, read, close */
#include <sys/types.h>    /* pid_t */
#include <sys/wait.h>     /* wait */
#include <sys/select.h>   /* pselect */
#include <fcntl.h> 

/* ---------------- CONSTANTES ---------------- */

#define NUM_PROC          3   /* quantos processos de aplicação (A1..A3) */
#define MAX_ITER          12  /* MAX: iterações de cada processo */
#define TIMESLICE         1   /* segundos entre duas IRQ0 */
#define IO_TIME           3   /* segundos entre duas IRQ1 */
#define INTERVALO_INICIO  1   /* A1 em 0 s, A2 em 1 s, A3 em 2 s... */
#define SIG_IRQ0          SIGUSR1   /* IRQ0 = fim de timeslice */
#define SIG_IRQ1          SIGUSR2   /* IRQ1 = fim de I/O em D1 */

/* Plano de syscalls: "PCtipo". Ex.: "3R" = no PC 3 faz read em D1 */
static const char *planos[NUM_PROC] = { "3R,7W", "4W,8R", "5R,9W" };

/* ---------------- TIPOS ---------------- */

/* Estados de um processo de aplicação */
typedef enum { NOVO, PRONTO, EXECUTANDO, BLOQUEADO, TERMINADO } Estado;

/* PCB: o que o KernelSim sabe sobre cada processo */
typedef struct {
    int    id;       /* 1 = A1, 2 = A2... */
    pid_t  pid;      /* pid Unix do filho */
    Estado estado;   /* NOVO, PRONTO, EXECUTANDO, BLOQUEADO ou TERMINADO */
    int    pc;       /* contexto: último PC informado (tempo de CPU) */
    int    chegada;  /* instante em que o processo entra no sistema */
    char   syscall_pendente;   /* 'R' ou 'W' enquanto espera D1; 0 caso contrário */
} PCB;

/* Fila circular de índices de processos (prontos ou espera de I/O) */
typedef struct {
    int itens[NUM_PROC];
    int inicio;       /* posição do primeiro elemento */
    int n;            /* quantos elementos há */
} Fila;

/* ---------------- VARIÁVEIS GLOBAIS ---------------- */

static PCB   tabela[NUM_PROC];  /* um PCB por processo */
static Fila  prontos;           /* fila de prontos (fila de execução) */
static Fila  espera_io;         /* fila de espera do dispositivo D1 */
static int   executando = -1;   /* índice do processo na CPU; -1 = ociosa */
static int   terminados = 0;    /* quantos já terminaram */
static int   tempo = 0;         /* relógio simulado, em segundos */
static int   pipe_leitura;      /* ponta de leitura do pipe */
static int req_escrita;        /* pipe de pedidos de I/O para o InterControllerSim */

/* Flags ligadas pelos tratadores de sinal (volatile sig_atomic_t é seguro) */
static volatile sig_atomic_t flag_irq0 = 0;
static volatile sig_atomic_t flag_irq1 = 0;

/* ---------------- LOG E MOSTRA ESTADO ---------------- */

/* Imprime uma linha com o instante simulado. Uso igual ao printf. */
static void log_evento(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("[t=%3ds] ", tempo);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);       /* mostra a linha na hora */
    va_end(ap);
}

/* Imprime uma fila como [A3 A1] na ordem em que sairão para a CPU */
static void imprime_fila(const char *rotulo, Fila *f) {
    printf("%s[", rotulo);
    for (int k = 0; k < f->n; k++) {
        int pos = (f->inicio + k) % NUM_PROC;
        printf("%sA%d", k ? " " : "", tabela[f->itens[pos]].id);
    }
    printf("]");
}

/* Linha de estado do segundo: CPU, fila de execução, fila de I/O e PC de cada um */
static void mostra_estado(void) {
    printf("   >> CPU: ");
    if (executando == -1) printf("ociosa");
    else                  printf("A%d", tabela[executando].id);

    printf(" | ");
    imprime_fila("Fila de execucao: ", &prontos);
    printf(" | ");
    imprime_fila("Fila I/O D1: ", &espera_io);

    printf(" | PC: ");
    for (int i = 0; i < NUM_PROC; i++)
        printf("A%d=%d%s", tabela[i].id, tabela[i].pc, i < NUM_PROC - 1 ? " " : "");
    printf("\n");
    fflush(stdout);
}

/* ---------------- OPERAÇÕES DE FILA ---------------- */

/* Coloca x no FIM da fila (fim de timeslice, bloqueio de I/O, chegada) */
static void fila_push_fim(Fila *f, int x) {
    f->itens[(f->inicio + f->n) % NUM_PROC] = x;
    f->n++;
}

/* Coloca x no INÍCIO da fila (I/O terminado: prioridade) */
static void fila_push_inicio(Fila *f, int x) {
    f->inicio = (f->inicio + NUM_PROC - 1) % NUM_PROC;
    f->itens[f->inicio] = x;
    f->n++;
}

/* Retira e devolve o primeiro da fila; -1 se estiver vazia */
static int fila_pop(Fila *f) {
    if (f->n == 0) return -1;
    int x = f->itens[f->inicio];
    f->inicio = (f->inicio + 1) % NUM_PROC;
    f->n--;
    return x;
}

/* Remove x do meio da fila, puxando os seguintes uma posição para trás */
static void fila_remove(Fila *f, int x) {
    for (int k = 0; k < f->n; k++) {
        int pos = (f->inicio + k) % NUM_PROC;
        if (f->itens[pos] == x) {
            for (int j = k; j < f->n - 1; j++)
                f->itens[(f->inicio + j) % NUM_PROC] =
                    f->itens[(f->inicio + j + 1) % NUM_PROC];
            f->n--;
            return;
        }
    }
}

/* ---------------- CONTROLE DOS PROCESSOS ---------------- */

/* Congela o processo i (SIGSTOP) */
static void suspende(int i) { kill(tabela[i].pid, SIGSTOP); }

/* Descongela o processo i (SIGCONT): ele volta de onde parou */
static void retoma(int i)   { kill(tabela[i].pid, SIGCONT); }

/* Se a CPU está livre, dá a CPU ao primeiro da fila de prontos */
static void escalona(void) {
    if (executando != -1) return;

    int prox = fila_pop(&prontos);
    if (prox == -1) {
        log_evento("CPU ociosa: nenhum processo pronto");
        return;
    }
    executando = prox;
    tabela[prox].estado = EXECUTANDO;
    retoma(prox);
    log_evento("ESCALONADO A%d (retomado com PC=%d)", tabela[prox].id, tabela[prox].pc);
}

/* Coloca no fim da fila de prontos os processos cujo instante de chegada já passou */
static void admite_chegados(void) {
    for (int i = 0; i < NUM_PROC; i++) {
        if (tabela[i].estado == NOVO && tabela[i].chegada <= tempo) {
            tabela[i].estado = PRONTO;
            fila_push_fim(&prontos, i);
            log_evento("A%d INICIA sua execucao -> fim da fila de prontos", tabela[i].id);
        }
    }
}

/* ---------------- TRATADORES DE EVENTO ---------------- */

/* IRQ0: fim do timeslice do processo que está na CPU */
static void trata_irq0(void) {
    tempo += TIMESLICE;
    log_evento("IRQ0: fim do timeslice");

    admite_chegados();

    if (executando != -1) {
        int i = executando;
        suspende(i);                       /* para o processo */
        tabela[i].estado = PRONTO;
        fila_push_fim(&prontos, i);        /* volta ao fim da fila */
        log_evento("A%d parado (timeslice esgotado, PC=%d) -> fim da fila de prontos",
                   tabela[i].id, tabela[i].pc);
        executando = -1;
    }
    escalona();
    mostra_estado();
}

/* IRQ1: D1 terminou o I/O do primeiro da fila de espera */
static void trata_irq1(void) {
    log_evento("IRQ1: I/O em D1 terminou");

    int i = fila_pop(&espera_io);
    if (i == -1) {
        log_evento("IRQ1 ignorado: ninguém esperava D1");
        return;
    }

    PCB *p = &tabela[i];
    log_evento("A%d desbloqueado (syscall D1 %c concluida, PC=%d) -> início da fila de prontos",
               p->id, p->syscall_pendente, p->pc);
    p->syscall_pendente = 0;                        /* syscall concluída: sai do contexto */
    p->estado = PRONTO;
    fila_push_inicio(&prontos, i);                  /* prioridade: início da fila */
    escalona();
}

/* Mensagem SYSCALL: o processo i pediu I/O em D1 no PC informado */
static void trata_syscall(int i, int pc, char tipo) {
    PCB *p = &tabela[i];
    if (p->estado == TERMINADO) return;

    p->pc = pc;
    p->syscall_pendente = tipo;                     /* parâmetro da syscall no contexto */
    if (p->estado == EXECUTANDO) executando = -1;
    else if (p->estado == PRONTO) fila_remove(&prontos, i);

    suspende(i);
    p->estado = BLOQUEADO;
    fila_push_fim(&espera_io, i);

    if (write(req_escrita, "R", 1) != 1)           /* avisa o InterControllerSim */
        perror("write");

    log_evento("A%d fez syscall D1 %c no PC=%d -> BLOQUEADO", p->id, tipo, pc);
    escalona();
}

/* Mensagem DONE: o processo i terminou as MAX iterações */
static void trata_fim(int i) {
    PCB *p = &tabela[i];
    if (p->estado == EXECUTANDO) executando = -1;
    else if (p->estado == PRONTO) fila_remove(&prontos, i);

    /* Se foi parado por IRQ0 logo após enviar DONE, precisa continuar para encerrar */
    retoma(i);

    p->estado = TERMINADO;
    terminados++;
    log_evento("A%d TERMINOU (PC final=%d)", p->id, p->pc);
    escalona();
}

/* Lê o pipe e interpreta as mensagens (uma por linha). Devolve 0 se o pipe fechou. */
static int ler_mensagens(void) {
    char buf[1024];
    ssize_t n = read(pipe_leitura, buf, sizeof buf - 1);
    if (n <= 0) return 0;
    buf[n] = '\0';

    char *linha = strtok(buf, "\n");
    while (linha != NULL) {
        int id, pc;
        char tipo;
        if (sscanf(linha, "PC %d %d", &id, &pc) == 2)
            tabela[id - 1].pc = pc;
        else if (sscanf(linha, "SYSCALL %d %d %c", &id, &pc, &tipo) == 3)
            trata_syscall(id - 1, pc, tipo);
        else if (sscanf(linha, "DONE %d", &id) == 1)
            trata_fim(id - 1);
        linha = strtok(NULL, "\n");
    }
    return 1;
}

/* Tratadores de sinal: só marcam a flag; o trabalho é feito no laço principal */
static void handler_irq0(int sig) { (void)sig; flag_irq0 = 1; }
static void handler_irq1(int sig) { (void)sig; flag_irq1 = 1; }

/* ---------------- INTERCONTROLLER SIM ---------------- */

/* Processo separado: IRQ0 a cada 1 s; IRQ1 IO_TIME s após o início de cada pedido de I/O */
static void intercontrolador(pid_t alvo, int fd_req) {
    int pendentes = 0;   /* pedidos ainda não concluídos (inclui o que está em curso) */
    int restante  = 0;   /* segundos que faltam para o I/O atual; 0 = D1 livre */
    char c;

    /* leitura não bloqueante dos pedidos: lê só o que já chegou */
    fcntl(fd_req, F_SETFL, fcntl(fd_req, F_GETFL) | O_NONBLOCK);

    while (1) {
        sleep(TIMESLICE);
        kill(alvo, SIG_IRQ0);                       /* IRQ0: fim do timeslice */

        while (read(fd_req, &c, 1) == 1)            /* novos pedidos de I/O em D1 */
            pendentes++;

        if (restante > 0 && --restante == 0) {      /* o I/O atual terminou */
            kill(alvo, SIG_IRQ1);                   /* IRQ1: fim de I/O em D1 */
            pendentes--;
        }
        if (restante == 0 && pendentes > 0)         /* D1 livre: começa o próximo */
            restante = IO_TIME;
    }
}

/* ---------------- MAIN (KERNEL SIM) ---------------- */

int main(void) {
    int fd[2], req[2];
    sigset_t bloqueio, antiga, vazia;
    struct sigaction sa;

    /* 1. Tratadores de IRQ0 e IRQ1 */
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handler_irq0;  sigaction(SIG_IRQ0, &sa, NULL);
    sa.sa_handler = handler_irq1;  sigaction(SIG_IRQ1, &sa, NULL);

    /* 2. Bloqueia IRQ0/IRQ1: só são recebidas dentro do pselect */
    sigemptyset(&bloqueio);
    sigaddset(&bloqueio, SIG_IRQ0);
    sigaddset(&bloqueio, SIG_IRQ1);
    sigprocmask(SIG_BLOCK, &bloqueio, &antiga);
    sigemptyset(&vazia);

    /* 3. Pipes: mensagens das aplicações e pedidos de I/O para o InterControllerSim */
    if (pipe(fd)  == -1) { perror("pipe"); exit(1); }
    if (pipe(req) == -1) { perror("pipe"); exit(1); }
    pipe_leitura = fd[0];
    req_escrita  = req[1];

    /* 4. InterControllerSim */
    pid_t irq_pid = fork();
    if (irq_pid == 0) {
        sigprocmask(SIG_SETMASK, &antiga, NULL);
        close(fd[0]);
        close(fd[1]);
        close(req_escrita);
        intercontrolador(getppid(), req[0]);
        _exit(0);
    }
    close(req[0]);   /* só o InterControllerSim lê os pedidos */

    /* 5. Processos de aplicação A1..An (nascem parados) */
    for (int i = 0; i < NUM_PROC; i++) {
        tabela[i].id              = i + 1;
        tabela[i].estado          = NOVO;
        tabela[i].pc              = 0;
        tabela[i].chegada         = i * INTERVALO_INICIO;
        tabela[i].syscall_pendente = 0;

        pid_t pid = fork();
        if (pid == 0) {
            sigprocmask(SIG_SETMASK, &antiga, NULL);
            close(fd[0]);
            close(req_escrita);
            char a_id[16], a_max[16], a_fd[16];
            snprintf(a_id,  sizeof a_id,  "%d", i + 1);
            snprintf(a_max, sizeof a_max, "%d", MAX_ITER);
            snprintf(a_fd,  sizeof a_fd,  "%d", fd[1]);
            execl("./app", "app", a_id, a_max, a_fd, planos[i], (char *)NULL);
            perror("execl");
            _exit(1);
        }
        tabela[i].pid = pid;
        suspende(i);
    }
    close(fd[1]);   /* KernelSim não escreve: assim o read dá EOF quando as apps saem */

    log_evento("KernelSim iniciado: %d processos, TIMESLICE=%ds, IO=%ds, MAX=%d",
               NUM_PROC, TIMESLICE, IO_TIME, MAX_ITER);

    admite_chegados();   /* A1 entra em t=0 */
    escalona();
    mostra_estado();

    /* 6. Laço principal: infinito, como pede o enunciado */
    int pipe_aberto = 1;
    int resumo_impresso = 0;

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (pipe_aberto) FD_SET(pipe_leitura, &rfds);

        int r = pselect(pipe_leitura + 1, &rfds, NULL, NULL, NULL, &vazia);
        if (r == -1 && errno != EINTR) { perror("pselect"); break; }

        if (flag_irq0) { flag_irq0 = 0; trata_irq0(); }
        if (flag_irq1) { flag_irq1 = 0; trata_irq1(); }

        if (pipe_aberto && r > 0 && FD_ISSET(pipe_leitura, &rfds))
            if (!ler_mensagens()) pipe_aberto = 0;

        if (terminados == NUM_PROC && !resumo_impresso) {
            resumo_impresso = 1;
            printf("\n===== RESUMO FINAL =====\n");
            for (int i = 0; i < NUM_PROC; i++)
                printf("A%d: PC final = %d, estado = %s\n", tabela[i].id, tabela[i].pc,
                       tabela[i].estado == TERMINADO ? "TERMINADO" : "NAO TERMINOU");
            fflush(stdout);
        }
    }
    return 0;
}