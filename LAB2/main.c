#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>

int main (int argc, char *argv[]) {

    int segmento, *p, id_filho, id_neto, pid, status;

    // aloca a memória compartilhada
    // S_IRUSR | S_IWUSR | S_IXUSR liga os 3 bits:
    // leitura, escrita e execução (para o dono)
    segmento = shmget (IPC_PRIVATE, sizeof (int), IPC_CREAT | IPC_EXCL | S_IRUSR | S_IWUSR | S_IXUSR);
    //                   chave        size          flags combinadas com or bit a bit
    
    if (segmento < 0) {
        puts ("Erro ao criar a memoria compartilhada");
        exit (-1);
    }

    // associa a memória compartilhada ao processo
    p = (int *) shmat (segmento, 0, 0);

    if (p == (int *) -1) {
        puts ("Erro no shmat");
        exit (-1);
    }

    *p = 1;   // valor inicial de n

    if ((id_filho = fork ()) < 0) {
        puts ("Erro na criacao do processo filho");
        exit (-2);
    }
    else if (id_filho == 0) {

        // processo filho
        if ((id_neto = fork ()) < 0) {
            puts ("Erro na criacao do processo neto");
            exit (-2);
        }
        else if (id_neto == 0) {

            // processo neto
            for (int i = 0; i < 1000; i++) {
                *p += 3;
                printf ("processo neto,  pid=%d, n=%d\n", getpid (), *p);
            }
            shmdt (p);   // libera a memoria compartilhada do processo neto
            exit (0);
        }
        else {
            // continuação do processo filho
            for (int i = 0; i < 1000; i++) {
                *p += 2;
                printf ("processo filho, pid=%d, n=%d\n", getpid (), *p);
            }

            pid = wait (&status); // espera o neto terminar

            shmdt (p);  // libera a memoria compartilhada do processo filho
            exit (0);
        }
    }
    else {
        // processo pai
        for (int i = 0; i < 1000; i++) {
            *p += 1;
            printf ("processo pai,   pid=%d, n=%d\n", getpid (), *p);
        }

        pid = wait (&status);   // espera o filho (e o neto) terminar
        // só chega aqui quando neto terminou e o filho também 

        shmdt (p);   // libera a memoria compartilhada do processo pai

        // libera/remove a memoria compartilhada do sistema
        shmctl (segmento, IPC_RMID, 0);
    }

    return 0;
}