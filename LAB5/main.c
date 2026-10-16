#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>

static pid_t p1, p2, p3;

// imprime uma mensagem prefixada com hh:mm:ss.uuuuuu, usando gettimeofday
static void log_msg(const char *msg) {
    struct timeval t;
    struct tm tmv;
    char buf[16];

    gettimeofday(&t, NULL);
    localtime_r(&t.tv_sec, &tmv);
    strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);

    printf("[%s.%06ld] %s\n", buf, (long)t.tv_usec, msg);
    fflush(stdout);
}

// código executado por cada processo filho
// o filho começa parado (SIGSTOP) e só roda quando o pai manda SIGCONT
// imprime seu ststais enquanto executa até o pai mandar SIGSTOP 
static void child_process(const char *name) {
    char msg[64];

    // garante que o filho comeca parado, esperando o pai liberar 
    raise(SIGSTOP);

    while (1) {
        snprintf(msg, sizeof(msg), "%s executando (pid=%d)", name, getpid());
        log_msg(msg);
        usleep(800000); // 800 ms entre mensagens, para nao poluir a saida 
    }
}

// "dorme" (usando nanosleep) ate o instante alvo em segundos/microssegundos
// reconsulta gettimeofday para nao acumular erro
static void sleep_until(time_t target_sec, long target_usec) {
    struct timeval now;
    struct timespec ts;
    long diff_us;

    while (1) {
        gettimeofday(&now, NULL);
        diff_us = (long)(target_sec - now.tv_sec) * 1000000L +
                  (target_usec - now.tv_usec);
        if (diff_us <= 0) return;

        ts.tv_sec  = diff_us / 1000000L;
        ts.tv_nsec = (diff_us % 1000000L) * 1000L;
        nanosleep(&ts, NULL);
    }
}

// manda SIGCONT para 'pid' e registra a mensagem
static void start_proc(pid_t pid, const char *name) {
    char msg[64];
    kill(pid, SIGCONT);
    snprintf(msg, sizeof(msg), ">>> Iniciando %s", name);
    log_msg(msg);
}

// manda SIGSTOP para 'pid' e registra a mensagem
static void stop_proc(pid_t pid, const char *name) {
    char msg[64];
    kill(pid, SIGSTOP);
    snprintf(msg, sizeof(msg), "<<< Parando %s", name);
    log_msg(msg);
}

static void cleanup_and_exit(int sig) {
    (void)sig;
    log_msg("Encerrando escalonador, matando filhos...");
    kill(p1, SIGCONT); kill(p1, SIGKILL);
    kill(p2, SIGCONT); kill(p2, SIGKILL);
    kill(p3, SIGCONT); kill(p3, SIGKILL);
    exit(0);
}

int main(void) {
    pid_t pid;
    struct timeval now;

    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);

    pid = fork();
    if (pid < 0) { perror("fork p1"); exit(1); }
    if (pid == 0) { child_process("P1"); exit(0); }
    p1 = pid;

    pid = fork();
    if (pid < 0) { perror("fork p2"); exit(1); }
    if (pid == 0) { child_process("P2"); exit(0); }
    p2 = pid;

    pid = fork();
    if (pid < 0) { perror("fork p3"); exit(1); }
    if (pid == 0) { child_process("P3"); exit(0); }
    p3 = pid;

    // espera os filhos chegarem no raise(SIGSTOP) antes do pai mandar sinais do controle
    usleep(200000);

    log_msg("Escalonador iniciado. P1: [5s-25s]  P2: [45s-60s]  P3: [0s-5s] e [25s-45s]");

    while (1) {
        time_t minute_start;

        // calcula o inicio do proximo minuto cheio (segundo == 0)
        gettimeofday(&now, NULL);
        minute_start = now.tv_sec - (now.tv_sec % 60) + 60;

        sleep_until(minute_start, 0);
        log_msg("=== Novo minuto iniciado ===");

        // Fase 1: 0s - 5s -> P3
        start_proc(p3, "P3");
        sleep_until(minute_start + 5, 0);
        stop_proc(p3, "P3");

        // Fase 2: 5s - 25s -> P1
        start_proc(p1, "P1");
        sleep_until(minute_start + 25, 0);
        stop_proc(p1, "P1");

        // Fase 3: 25s - 45s -> P3
        start_proc(p3, "P3");
        sleep_until(minute_start + 45, 0);
        stop_proc(p3, "P3");

        // Fase 4: 45s - 60s -> P2
        start_proc(p2, "P2");
        sleep_until(minute_start + 60, 0);
        stop_proc(p2, "P2");
    }

    return 0;
}