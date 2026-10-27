/*
 * =============================================================================
 *  PARTE 1 - Troca de mensagens ASSINCRONA entre processos
 *            (1 produtor + 2 consumidores, fila com capacidade de 64 elementos)
 * =============================================================================
 *
 * O QUE O PROGRAMA FAZ
 *   - PRODUTOR     : gera um inteiro a cada 1 s e o coloca na fila de dados.
 *   - CONSUMIDOR 1 : retira um inteiro da fila a cada 1 s.
 *   - CONSUMIDOR 2 : retira um inteiro da fila a cada 2 s.
 *   - Juntos, produtor e consumidores tratam exatamente 256 elementos.
 *   - A fila de dados nunca contem mais de 64 elementos.
 *
 * RECURSOS COMPARTILHADOS (criados pelo processo pai antes do fork)
 *   fila_dados : fila de mensagens System V onde ficam os inteiros produzidos.
 *   fila_vagas : fila de mensagens usada como controle de vagas ("tokens").
 *                Comeca com 64 tokens. O produtor retira um token antes de
 *                enviar um dado; o consumidor devolve o token depois de retirar
 *                um dado. Sem tokens, o produtor BLOQUEIA (fila cheia).
 *                Isso implementa um buffer limitado de 64 posicoes.
 *   pipe       : canal pelo qual cada filho informa ao pai o seu resumo final
 *                (quantidade e soma). Usado apenas na verificacao final.
 *
 * POR QUE E ASSINCRONA?
 *   O produtor nao espera o consumidor retirar a mensagem: ele deposita o dado
 *   na fila e segue em frente (enquanto houver vaga). O consumidor tambem nao
 *   obriga o produtor a esperar por ele. A fila desacopla os dois ritmos.
 *
 * ENCERRAMENTO
 *   Apos a ultima mensagem de dado, o produtor envia 2 mensagens de FIM (uma
 *   para cada consumidor). A fila e FIFO, entao cada consumidor so recebe um
 *   FIM depois de ja ter recebido todos os dados que vieram antes dele.
 *
 * COMPILAR / EXECUTAR
 *   gcc -Wall -Wextra -o parte1 parte1_assincrona.c
 *   ./parte1              -> tempo real (1 s por acao, aprox. 4,5 minutos)
 *   ./parte1 0.05         -> fator de escala: todos os tempos x 0,05 (teste rapido)
 * =============================================================================
 */

#include <stdio.h>       /* printf, perror, snprintf                              */
#include <stdlib.h>      /* exit, rand, srand, atof                               */
#include <string.h>      /* strcpy                                                */
#include <stdarg.h>      /* va_list: funcao de log com argumentos variaveis       */
#include <time.h>        /* time, clock_gettime, nanosleep                        */
#include <unistd.h>      /* fork, getpid, pipe, read, write, close                */
#include <sys/types.h>   /* pid_t                                                 */
#include <sys/ipc.h>     /* IPC_PRIVATE, IPC_CREAT, IPC_STAT, IPC_SET, IPC_RMID   */
#include <sys/msg.h>     /* msgget, msgsnd, msgrcv, msgctl, struct msqid_ds       */
#include <sys/wait.h>    /* wait                                                  */

/* ---------------- Parametros do problema ---------------- */
#define TAM_FILA          64     /* capacidade maxima da fila (elementos)         */
#define TOTAL_MENSAGENS   256    /* produzidos = consumidos = 256                 */
#define NUM_CONSUMIDORES  2

/* ---------------- Tipos de mensagem (campo mtype) ---------------- */
#define TIPO_DADO  1L    /* mensagem com um inteiro produzido                     */
#define TIPO_FIM   2L    /* sinal "nao ha mais dados" (um para cada consumidor)   */
#define TIPO_VAGA  3L    /* token: representa uma posicao livre na fila           */

/*
 * Estrutura trocada pelas filas. Exigencia da API System V: o PRIMEIRO campo
 * deve ser um long com o tipo da mensagem (mtype).
 */
struct mensagem {
    long mtype;    /* tipo da mensagem (o msgrcv usa para escolher)               */
    int  valor;    /* inteiro gerado pelo produtor                                */
    int  seq;      /* numero sequencial da mensagem (1..256)                      */
};

/*
 * Tamanho do "texto" da mensagem = tamanho total MENOS o campo mtype.
 * Este e o valor passado como tamanho para msgsnd() e msgrcv().
 */
#define TAM_CORPO (sizeof(struct mensagem) - sizeof(long))

/* Resumo que cada filho escreve no pipe antes de terminar. */
struct resumo {
    char papel[16];     /* nome do processo: "PRODUTOR", "CONSUMIDOR1", ...       */
    int  quantidade;    /* quantas mensagens produziu / consumiu                  */
    long soma;          /* soma dos valores, para conferencia                     */
};

/* ---------------- Variaveis globais ---------------- */
static double escala = 1.0;       /* multiplica todos os tempos de espera          */
static struct timespec inicio;    /* instante de inicio do programa (para o log)   */

/* ============================================================================
 *  Funcoes auxiliares
 * ============================================================================ */

/* Segundos decorridos desde o inicio do programa (relogio monotonico). */
static double tempo_decorrido(void) {
    struct timespec agora;
    clock_gettime(CLOCK_MONOTONIC, &agora);
    return (agora.tv_sec - inicio.tv_sec) + (agora.tv_nsec - inicio.tv_nsec) / 1e9;
}

/*
 * Imprime uma linha de log com: instante, PID, papel do processo e o texto.
 * fflush() garante que as linhas de processos diferentes nao fiquem presas
 * em buffers separados (cada processo tem sua propria copia do stdout).
 */
static void registra(const char *papel, const char *fmt, ...) {
    va_list ap;

    printf("[t=%8.3fs] pid=%-6d %-12s | ", tempo_decorrido(), (int)getpid(), papel);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

/* Espera 'segundos' (ajustado pela escala). nanosleep e portavel e preciso. */
static void espera(double segundos) {
    struct timespec ts;
    double real = segundos * escala;

    ts.tv_sec  = (time_t)real;
    ts.tv_nsec = (long)((real - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

/*
 * Retorna quantas mensagens estao atualmente na fila (campo msg_qnum).
 * Usado apenas para registrar o estado da fila nos logs.
 */
static int ocupacao_fila(int qid) {
    struct msqid_ds ds;

    if (msgctl(qid, IPC_STAT, &ds) == -1) {
        perror("msgctl(IPC_STAT)");
        return -1;
    }
    return (int)ds.msg_qnum;
}

/*
 * Configura a fila ANTES de usa-la:
 *   - le a configuracao padrao (IPC_STAT);
 *   - limita msg_qbytes (capacidade em bytes da fila) a exatamente 64 mensagens.
 *     Como cada mensagem tem TAM_CORPO bytes, o limite e TAM_FILA * TAM_CORPO.
 *     Assim o proprio kernel tambem impede mais de 64 mensagens na fila.
 *   - aplica a nova configuracao (IPC_SET).
 * Obs.: reduzir msg_qbytes e permitido ao dono da fila; aumentar alem de
 *       MSGMNB exige privilegio de superusuario.
 */
static void configura_fila(int qid, const char *nome) {
    struct msqid_ds ds;

    if (msgctl(qid, IPC_STAT, &ds) == -1) {
        perror("configura_fila: IPC_STAT");
        exit(1);
    }
    printf("Configuracao padrao de %-10s: msg_qbytes=%lu bytes, permissoes=%o\n",
           nome, (unsigned long)ds.msg_qbytes, (unsigned)(ds.msg_perm.mode & 0777));

    ds.msg_qbytes = TAM_FILA * TAM_CORPO;          /* 64 mensagens de TAM_CORPO bytes */
    if (msgctl(qid, IPC_SET, &ds) == -1) {
        perror("configura_fila: IPC_SET");
        exit(1);
    }
    printf("Nova configuracao      de %-10s: msg_qbytes=%lu bytes (= %d mensagens)\n",
           nome, (unsigned long)ds.msg_qbytes, TAM_FILA);
}

/* Retira um token (vaga) da fila de vagas. BLOQUEIA se nao houver nenhum. */
static void reserva_vaga(int fila_vagas) {
    struct mensagem tok;

    if (msgrcv(fila_vagas, &tok, TAM_CORPO, 0, 0) == -1) {
        perror("reserva_vaga: msgrcv");
        exit(1);
    }
}

/* Devolve um token (vaga) para a fila de vagas. Nunca bloqueia na pratica. */
static void devolve_vaga(int fila_vagas) {
    struct mensagem tok;

    tok.mtype = TIPO_VAGA;
    tok.valor = 0;
    tok.seq   = 0;
    if (msgsnd(fila_vagas, &tok, TAM_CORPO, 0) == -1) {
        perror("devolve_vaga: msgsnd");
        exit(1);
    }
}

/* ============================================================================
 *  Processo PRODUTOR
 * ============================================================================ */
static void processo_produtor(int fila_dados, int fila_vagas, int pipe_escrita) {
    struct mensagem m;
    struct resumo   r;
    long soma = 0;
    int  i, k;

    srand((unsigned)(getpid() ^ time(NULL)));   /* semente diferente por processo */

    strcpy(r.papel, "PRODUTOR");
    r.quantidade = 0;

    registra("PRODUTOR", "iniciado: vai produzir %d inteiros, 1 a cada 1 s",
             TOTAL_MENSAGENS);

    for (i = 1; i <= TOTAL_MENSAGENS; i++) {
        int valor;

        espera(1.0);                      /* ritmo de producao: 1 valor por segundo */
        valor = rand() % 1000;            /* inteiro gerado: 0..999                 */

        /* Se a fila ja esta cheia, o produtor vai bloquear ate uma vaga surgir. */
        if (ocupacao_fila(fila_dados) >= TAM_FILA)
            registra("PRODUTOR", "fila CHEIA (%d/%d): aguardando vaga...",
                     TAM_FILA, TAM_FILA);

        reserva_vaga(fila_vagas);         /* bloqueia se nao houver vaga           */

        m.mtype = TIPO_DADO;
        m.valor = valor;
        m.seq   = i;
        if (msgsnd(fila_dados, &m, TAM_CORPO, 0) == -1) {
            perror("PRODUTOR: msgsnd");
            exit(1);
        }

        soma += valor;
        r.quantidade++;
        registra("PRODUTOR", "produziu seq=%3d valor=%3d | fila=%2d/%d",
                 i, valor, ocupacao_fila(fila_dados), TAM_FILA);
    }

    /* Envia um sinal de FIM para cada consumidor (tambem ocupa uma vaga). */
    for (k = 0; k < NUM_CONSUMIDORES; k++) {
        reserva_vaga(fila_vagas);
        m.mtype = TIPO_FIM;
        m.valor = 0;
        m.seq   = 0;
        if (msgsnd(fila_dados, &m, TAM_CORPO, 0) == -1) {
            perror("PRODUTOR: msgsnd (FIM)");
            exit(1);
        }
    }
    registra("PRODUTOR", "terminou: %d dados produzidos, soma=%ld, sinais de FIM enviados",
             r.quantidade, soma);

    r.soma = soma;
    if (write(pipe_escrita, &r, sizeof r) != (ssize_t)sizeof r)
        perror("PRODUTOR: write(pipe)");
    close(pipe_escrita);
}

/* ============================================================================
 *  Processo CONSUMIDOR (usado com periodo 1 s e com periodo 2 s)
 * ============================================================================ */
static void processo_consumidor(int id, double periodo, int fila_dados, 
                                int fila_vagas, int pipe_escrita) {
    struct mensagem m;
    struct resumo   r;
    char nome[16];

    snprintf(nome, sizeof nome, "CONSUMIDOR%d", id);
    snprintf(r.papel, sizeof r.papel, "%s", nome);
    r.quantidade = 0;
    r.soma       = 0;

    registra(nome, "iniciado: retira 1 mensagem a cada %.0f s", periodo);

    for (;;) {
        /* Se a fila esta vazia, msgrcv vai bloquear ate chegar um dado. */
        if (ocupacao_fila(fila_dados) == 0)
            registra(nome, "fila vazia: aguardando mensagem...");

        /* msgtyp = 0: pega a primeira mensagem da fila, de qualquer tipo (FIFO). */
        if (msgrcv(fila_dados, &m, TAM_CORPO, 0, 0) == -1) {
            perror("consumidor: msgrcv");
            exit(1);
        }

        if (m.mtype == TIPO_FIM) {          /* nao ha mais dados: encerra */
            devolve_vaga(fila_vagas);
            registra(nome, "recebeu sinal de FIM: encerrando");
            break;
        }

        r.quantidade++;
        r.soma += m.valor;
        registra(nome, "consumiu seq=%3d valor=%3d | fila=%2d/%d",
                 m.seq, m.valor, ocupacao_fila(fila_dados), TAM_FILA);

        devolve_vaga(fila_vagas);           /* libera a posicao para o produtor */
        espera(periodo);                    /* ritmo de consumo                 */
    }

    registra(nome, "terminou: consumiu %d mensagens, soma=%ld", r.quantidade, r.soma);

    if (write(pipe_escrita, &r, sizeof r) != (ssize_t)sizeof r)
        perror("consumidor: write(pipe)");
    close(pipe_escrita);
}

/* ============================================================================
 *  Processo PAI: cria recursos, dispara os 3 processos, espera e verifica.
 * ============================================================================ */
int main(int argc, char *argv[]) {
    int fila_dados, fila_vagas;
    int p[2];                      /* pipe: p[0] leitura (pai), p[1] escrita (filhos) */
    pid_t pid;
    int i;

    if (argc > 1) {
        escala = atof(argv[1]);
        if (escala <= 0.0)
            escala = 1.0;
    }
    clock_gettime(CLOCK_MONOTONIC, &inicio);

    printf("=== PARTE 1: troca assincrona (1 produtor, 2 consumidores) ===\n");
    printf("Fator de escala de tempo: %.3f (1.000 = tempo real)\n\n", escala);

    /* 1) Criacao das filas. IPC_PRIVATE gera uma chave exclusiva;
     *    IPC_CREAT cria a fila; 0600 = leitura/escrita so para o dono.      */
    fila_dados = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    fila_vagas = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    if (fila_dados == -1 || fila_vagas == -1) {
        perror("msgget");
        exit(1);
    }

    /* 2) Configuracao das filas antes de usa-las. */
    configura_fila(fila_dados, "fila_dados");
    configura_fila(fila_vagas, "fila_vagas");

    /* 3) Preenche a fila de vagas com 64 tokens: 64 posicoes livres. */
    for (i = 0; i < TAM_FILA; i++)
        devolve_vaga(fila_vagas);
    printf("Fila de vagas preenchida com %d tokens (posicoes livres)\n\n", TAM_FILA);

    /* 4) Pipe para os resumos. */
    if (pipe(p) == -1) {
        perror("pipe");
        exit(1);
    }

    fflush(stdout);   /* evita duplicar texto ja impresso no buffer dos filhos */

    /* 5) Processo PRODUTOR. */
    pid = fork();
    if (pid == -1) { perror("fork"); exit(1); }
    if (pid == 0) {
        close(p[0]);
        processo_produtor(fila_dados, fila_vagas, p[1]);
        exit(0);
    }

    /* 6) Processo CONSUMIDOR 1: retira a cada 1 s. */
    pid = fork();
    if (pid == -1) { perror("fork"); exit(1); }
    if (pid == 0) {
        close(p[0]);
        processo_consumidor(1, 1.0, fila_dados, fila_vagas, p[1]);
        exit(0);
    }

    /* 7) Processo CONSUMIDOR 2: retira a cada 2 s. */
    pid = fork();
    if (pid == -1) { perror("fork"); exit(1); }
    if (pid == 0) {
        close(p[0]);
        processo_consumidor(2, 2.0, fila_dados, fila_vagas, p[1]);
        exit(0);
    }

    /* 8) PAI: fecha a escrita do pipe (so os filhos escrevem) e espera os 3 filhos. */
    close(p[1]);
    for (i = 0; i < 1 + NUM_CONSUMIDORES; i++)
        wait(NULL);

    /* 9) Le os resumos enviados pelos filhos e faz a verificacao final. */
    {
        struct resumo r;
        int total_prod = 0, total_cons = 0;
        long soma_prod = 0, soma_cons = 0;

        printf("\n=== RESUMO FINAL ===\n");
        while (read(p[0], &r, sizeof r) == (ssize_t)sizeof r) {
            printf("  %-12s : %3d mensagens, soma=%ld\n", r.papel, r.quantidade, r.soma);
            if (strcmp(r.papel, "PRODUTOR") == 0) {
                total_prod += r.quantidade;
                soma_prod  += r.soma;
            } else {
                total_cons += r.quantidade;
                soma_cons  += r.soma;
            }
        }
        close(p[0]);

        printf("  Total produzido : %d (esperado %d)\n", total_prod, TOTAL_MENSAGENS);
        printf("  Total consumido : %d (esperado %d)\n", total_cons, TOTAL_MENSAGENS);
        printf("  Soma produzida  : %ld\n", soma_prod);
        printf("  Soma consumida  : %ld\n", soma_cons);
        printf("  VERIFICACAO     : %s\n",
               (total_prod == TOTAL_MENSAGENS && total_cons == TOTAL_MENSAGENS &&
                soma_prod == soma_cons) ? "OK - todos os dados foram transferidos"
                                        : "ERRO - dados inconsistentes");
    }

    /* 10) Remocao das filas do sistema (IPC_RMID). Sem isso elas ficariam
     *     alocadas no kernel depois que o programa termina.                */
    if (msgctl(fila_dados, IPC_RMID, NULL) == -1)
        perror("msgctl(IPC_RMID) fila_dados");
    if (msgctl(fila_vagas, IPC_RMID, NULL) == -1)
        perror("msgctl(IPC_RMID) fila_vagas");
    printf("Filas removidas do sistema.\n");

    return 0;
}