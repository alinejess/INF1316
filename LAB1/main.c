// Aline Jéssica David Gonçalves, 2320276
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main(void) {
    int n = 1;
    int i;
    pid_t pid_filho, pid_neto; // o atual já é o pai

    pid_filho = fork();

    // chamada do filho
    if (pid_filho == 0) { // igualamos a zero, pois queremos o pid do filho e != 0 seria o pai
        pid_neto = fork();

        if (pid_neto == 0) {
            // este bloco só é executado pelo neto
            for (i = 0; i < 1000; i++) {
                n = n + 3; // incremento de 3 pelo neto
            }
            printf("processo neto, pid=%d, n=%d\n", getpid(), n);
            exit(0);
            
        } else { // aqui entra se pid_neto != 1
            // este bloco é executado pelo filho (continuação)
            for (i = 0; i < 1000; i++) {
                n = n + 2; // incremento de 2 pelo filho
            }
            printf("processo filho, pid=%d, n=%d\n", getpid(), n);
            wait(NULL);  // espera o neto terminar
            exit(0);
        }

    } else { // aqui entra se pid_filho != 0 
        for (i = 0; i < 1000; i++) {
            n = n + 1; // incremento de 1 pelo pai
        }
        printf("processo pai, pid=%d, n=%d\n", getpid(), n);
        wait(NULL);
    }

    return 0;
}
