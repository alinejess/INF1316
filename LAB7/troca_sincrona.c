/*
 * =============================================================================
 *  PARTE 2 - Troca de mensagens SINCRONA entre 2 processos
 *            (1 emissor + 1 receptor, 32 mensagens)
 * =============================================================================
 *
 * O QUE O PROGRAMA FAZ
 *   - EMISSOR  : gera um inteiro e o envia ao receptor; em seguida FICA
 *                BLOQUEADO ate receber a confirmacao. So depois envia o proximo.
 *   - RECEPTOR : fica BLOQUEADO esperando uma mensagem, processa o inteiro
 *                (simulado com uma pausa) e envia a confirmacao ao emissor.
 *   - Sao geradas e consumidas 32 mensagens.
 *
 * O QUE SIGNIFICA "SINCRONA" NESTE PROGRAMA
 *   Na comunicacao sincrona (rendez-vous), o emissor so continua depois que a
 *   mensagem foi efetivamente recebida, e o receptor so continua depois que a
 *   mensagem chegou. A fila de mensagens do System V, por si so, e assincrona
 *   (msgsnd retorna assim que a mensagem entra na fila). Por isso a sincronia
 *   e obtida com um protocolo de confirmacao (ACK):
 *       emissor  : msgsnd(DADO) -> msgrcv(CONFIRMA)   <- bloqueia aqui
 *       receptor : msgrcv(DADO) -> processa -> msgsnd(CONFIRMA)
 *   Assim, no maximo UMA mensagem existe na fila em cada instante.
 *
 * RECURSOS
 *   fila : uma unica fila de mensagens System V. Os tipos (mtype) separam os
 *          dois sentidos da comunicacao:
 *            TIPO_DADO      (1) : emissor -> receptor
 *            TIPO_CONFIRMA  (2) : receptor -> emissor
 *          Cada processo so retira o tipo que lhe interessa, entao nunca
 *          "pega" a propria mensagem.
 *
 * COMPILAR / EXECUTAR
 *   gcc -Wall -Wextra -o parte2 parte2_sincrona.c
 *   ./parte2
 * =============================================================================
 */

#include <stdio.h>       /* printf, perror                                        */
#include <stdlib.h>      /* exit, rand, srand                                     */
#include <time.h>        /* time, clock_gettime, nanosleep                        */
#include <unistd.h>      /* fork, getpid                                          */
#include <sys/types.h>   /* pid_t                                                 */
#include <sys/ipc.h>     /* IPC_PRIVATE, IPC_CREAT, IPC_STAT, IPC_SET, IPC_RMID   */
#include <sys/msg.h>     /* msgget, msgsnd, msgrcv, msgctl, struct msqid_ds       */
#include <sys/wait.h>    /* wait                                                  */
#include <stdarg.h>      /* va_list                                               */

/* ---------------- Parametros ---------------- */
#define TOTAL_MENSAGENS  32     /* quantidade de mensagens geradas e consumidas   */
#define TEMPO_PROCESSO   0.5    /* tempo (s) que o receptor leva "processando"    */
#define TEMPO_GERACAO   0.3     /* tempo (s) que o emissor leva para gerar o dado */

/* ---------------- Tipos de mensagem (mtype) ---------------- */
#define TIPO_DADO      1L       /* emissor -> receptor: carrega o inteiro         */
#define TIPO_CONFIRMA  2L       /* receptor -> emissor: confirma o recebimento    */

/* Estrutura trocada pela fila. O primeiro campo DEVE ser long mtype. */
struct mensagem {
    long mtype;    /* tipo da mensagem (escolhe quem deve receber)                */
    int  valor;    /* inteiro gerado pelo emissor                                 */
    int  seq;      /* numero sequencial da mensagem (1..32)                       */
};

/* Tamanho do corpo da mensagem (sem o mtype), usado em msgsnd/msgrcv. */
#define TAM_CORPO (sizeof(struct mensagem) - sizeof(long))

static struct timespec inicio;   /* instante de inicio, para o log */

/* ============================================================================
 *  Funcoes auxiliares
 * ============================================================================ */

static double tempo_decorrido(void) {
    struct timespec agora;
    clock_gettime(CLOCK_MONOTONIC, &agora);
    return (agora.tv_sec - inicio.tv_sec) + (agora.tv_nsec - inicio.tv_nsec) / 1e9;
}

/* Log com instante, PID, papel e texto. fflush mantem a ordem real das linhas. */
static void registra(const char *papel, const char *fmt, ...) {
    va_list ap;

    printf("[t=%8.3fs] pid=%-6d %-9s | ", tempo_decorrido(), (int)getpid(), papel);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* Pausa de 'segundos' (com precisao de nanosegundos). */
static void espera(double segundos) {
    struct timespec ts;

    ts.tv_sec  = (time_t)segundos;
    ts.tv_nsec = (long)((segundos - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

/* Numero de mensagens atualmente na fila (apenas para o log). */
static int ocupacao_fila(int qid) {
    struct msqid_ds ds;

    if (msgctl(qid, IPC_STAT, &ds) == -1) {
        perror("msgctl(IPC_STAT)");
        return -1;
    }
    return (int)ds.msg_qnum;
}

/*
 * Configura a fila antes do uso:
 *   msg_qbytes = 1 mensagem. Como o protocolo e estritamente alternado
 *   (dado, confirmacao, dado, confirmacao...), nunca ha duas mensagens ao mesmo
 *   tempo na fila; limitar a capacidade a uma mensagem torna isso explicito e
 *   garantido pelo proprio kernel.
 */
static void configura_fila(int qid) {
    struct msqid_ds ds;

    if (msgctl(qid, IPC_STAT, &ds) == -1) {
        perror("configura_fila: IPC_STAT");
        exit(1);
    }
    printf("Configuracao padrao: msg_qbytes=%lu bytes, permissoes=%o\n",
           (unsigned long)ds.msg_qbytes, (unsigned)(ds.msg_perm.mode & 0777));

    ds.msg_qbytes = TAM_CORPO;               /* capacidade: 1 mensagem */
    if (msgctl(qid, IPC_SET, &ds) == -1) {
        perror("configura_fila: IPC_SET");
        exit(1);
    }
    printf("Nova configuracao : msg_qbytes=%lu bytes (= 1 mensagem)\n\n",
           (unsigned long)ds.msg_qbytes);
}

/* ============================================================================
 *  Processo EMISSOR
 * ============================================================================ */
static void processo_emissor(int fila) {
    struct mensagem m;
    int  i;

    srand((unsigned)(getpid() ^ time(NULL)));

    registra("EMISSOR", "iniciado: vai enviar %d mensagens de forma sincrona",
             TOTAL_MENSAGENS);

    for (i = 1; i <= TOTAL_MENSAGENS; i++) {
        struct timespec t0, t1;
        double bloqueado;

        espera(TEMPO_GERACAO);                 /* "gera" o inteiro */
        m.mtype = TIPO_DADO;
        m.valor = rand() % 1000;               /* inteiro: 0..999  */
        m.seq   = i;

        /* ---- Envio ---- */
        registra("EMISSOR", "envia seq=%2d valor=%3d", m.seq, m.valor);
        if (msgsnd(fila, &m, TAM_CORPO, 0) == -1) {
            perror("EMISSOR: msgsnd");
            exit(1);
        }

        /* ---- Sincronia: aguarda a confirmacao do receptor ---- */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (msgrcv(fila, &m, TAM_CORPO, TIPO_CONFIRMA, 0) == -1) {
            perror("EMISSOR: msgrcv (confirmacao)");
            exit(1);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        bloqueado = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

        registra("EMISSOR", "confirmacao da seq=%2d recebida (ficou bloqueado %.3f s)",
                 m.seq, bloqueado);
    }

    registra("EMISSOR", "terminou: %d mensagens enviadas e confirmadas", TOTAL_MENSAGENS);
}

/* ============================================================================
 *  Processo RECEPTOR
 * ============================================================================ */
static void processo_receptor(int fila) {
    struct mensagem m;
    long soma = 0;
    int  i;

    registra("RECEPTOR", "iniciado: vai receber %d mensagens", TOTAL_MENSAGENS);

    for (i = 1; i <= TOTAL_MENSAGENS; i++) {
        /* Se nao ha mensagem, msgrcv bloqueia ate o emissor enviar. */
        if (ocupacao_fila(fila) == 0)
            registra("RECEPTOR", "aguardando seq=%2d (fila vazia: bloqueado)", i);

        /* Recebe somente mensagens do tipo DADO. */
        if (msgrcv(fila, &m, TAM_CORPO, TIPO_DADO, 0) == -1) {
            perror("RECEPTOR: msgrcv");
            exit(1);
        }
        soma += m.valor;
        registra("RECEPTOR", "recebeu seq=%2d valor=%3d", m.seq, m.valor);

        espera(TEMPO_PROCESSO);                /* "processa" a mensagem */

        /* ---- Confirmacao: libera o emissor ---- */
        m.mtype = TIPO_CONFIRMA;               /* m.seq ja identifica a mensagem */
        if (msgsnd(fila, &m, TAM_CORPO, 0) == -1) {
            perror("RECEPTOR: msgsnd (confirmacao)");
            exit(1);
        }
        registra("RECEPTOR", "processou seq=%2d e enviou confirmacao", m.seq);
    }

    registra("RECEPTOR", "terminou: %d mensagens recebidas, soma=%ld",
             TOTAL_MENSAGENS, soma);
}

/* ============================================================================
 *  Processo PAI: cria a fila, dispara emissor e receptor, espera e remove a fila.
 * ============================================================================ */
int main(void) {
    int fila;
    pid_t pid_emissor, pid_receptor;

    clock_gettime(CLOCK_MONOTONIC, &inicio);

    printf("=== PARTE 2: troca sincrona (1 emissor, 1 receptor, %d mensagens) ===\n\n",
           TOTAL_MENSAGENS);

    /* 1) Criacao da fila (IPC_PRIVATE: chave exclusiva; 0600: so o dono acessa). */
    fila = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (fila == -1) {
        perror("msgget");
        exit(1);
    }

    /* 2) Configuracao da fila antes do uso. */
    configura_fila(fila);

    fflush(stdout);

    /* 3) Processo RECEPTOR. */
    pid_receptor = fork();
    if (pid_receptor == -1) { perror("fork"); exit(1); }
    if (pid_receptor == 0) {
        processo_receptor(fila);
        exit(0);
    }

    /* 4) Processo EMISSOR. */
    pid_emissor = fork();
    if (pid_emissor == -1) { perror("fork"); exit(1); }
    if (pid_emissor == 0) {
        processo_emissor(fila);
        exit(0);
    }

    /* 5) PAI: espera os dois processos terminarem. */
    waitpid(pid_emissor, NULL, 0);
    waitpid(pid_receptor, NULL, 0);

    /* 6) Estado final e remocao da fila. */
    printf("\nMensagens restantes na fila ao final: %d\n", ocupacao_fila(fila));
    if (msgctl(fila, IPC_RMID, NULL) == -1)
        perror("msgctl(IPC_RMID)");
    printf("Fila removida do sistema.\n");

    return 0;
}