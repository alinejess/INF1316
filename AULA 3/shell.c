#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define TRUE 1
#define MAX_LINE 100

void type_prompt() {
    printf("minishell> ");
    fflush(stdout);
}

void read_command(char *command, char *parameters[]) {
    char line[MAX_LINE];
    fgets(line, MAX_LINE, stdin);
    line[strcspn(line, "\n")] = '\0';  // remove o \n do final

    int i = 0;
    char *token = strtok(line, " ");
    while (token != NULL) {
        parameters[i] = token;
        i++;
        token = strtok(NULL, " ");
    }
    parameters[i] = NULL;  // execve exige NULL no final

    strcpy(command, parameters[0]);  // primeiro token é o comando
}

int main() {
    char command[MAX_LINE];
    char *parameters[20];
    int status;

    while (TRUE) {
        type_prompt();
        read_command(command, parameters);

        if (fork() != 0) {
            waitpid(-1, &status, 0);
        } else {
            execve(command, parameters, NULL);
            perror("execve falhou");  // só chega aqui se der erro
            exit(1);
        }
    }
    return 0;
}