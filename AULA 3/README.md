## Execução
cd "AULA 3"
gcc echo.c -o echo
pwd
ls
gcc shell.c -o shell

## Como testar
./shell
./echo coisa boa

# Entendendo um Mini-Shell em C: `fork()`, `execve()` e o programa `echo`

Este artigo explica, passo a passo, como dois programas C separados — um **mini-shell** e um programa **echo** — trabalham juntos no sistema operacional, desde a escrita do código até a execução final no terminal.

---

## 1. Visão geral: dois programas, dois papéis

Antes de entrar no código, é importante entender a arquitetura:

| Programa | Papel | O que faz |
|---|---|---|
| `shell.c` | Processo **pai** (interpretador de comandos) | Lê o que o usuário digita, cria um processo filho e espera ele terminar |
| `echo.c` | Processo **filho** (programa executado) | Recebe argumentos e os imprime na tela |

Eles são **compilados separadamente** e viram **dois executáveis independentes** (`shell` e `echo`). O `shell` não "contém" o código do `echo` — ele apenas manda o sistema operacional *carregar e rodar* o executável `echo` quando o usuário pede.

---

## 2. O programa `echo.c`

### 2.1 O código

```c
#include <stdio.h>
int main(int argc, char *argv[]) {
    int i;
    for (i = 1; i < argc; i++) {
        printf("%s", argv[i]);
        if (i < argc - 1) {
            printf(" ");
        }
    }
    printf("\n");
    return 0;
}
```

### 2.2 Como o `main` recebe seus argumentos

Todo programa C que roda a partir de um shell é iniciado assim:

```c
int main(int argc, char *argv[])
```

- **`argc`** (*argument count*): quantos "pedaços" (tokens) foram passados na linha de comando, contando o próprio nome do programa.
- **`argv`** (*argument vector*): um vetor de strings com cada um desses pedaços. `argv[0]` é sempre o nome do programa.

Exemplo: se o programa for chamado como `./echo ola mundo`, o sistema operacional monta:

```
argc = 3
argv[0] = "./echo"
argv[1] = "ola"
argv[2] = "mundo"
argv[3] = NULL   // sempre terminado em NULL
```

### 2.3 O que o `for` faz

```c
for (i = 1; i < argc; i++) {
    printf("%s", argv[i]);
    if (i < argc - 1) {
        printf(" ");
    }
}
printf("\n");
```

- O laço começa em `i = 1` (pula `argv[0]`, que é só o nome do programa, não um argumento "de verdade").
- Imprime cada argumento com `printf("%s", argv[i])`.
- Se **não** for o último argumento, imprime um espaço depois — isso evita um espaço sobrando no final da linha.
- Depois do laço, imprime uma quebra de linha (`\n`).

Com `argv[1] = "ola"` e `argv[2] = "mundo"`, a saída é:

```
ola mundo
```

### 2.4 Compilando o `echo.c`

```bash
gcc echo.c -o echo
```

O que este comando faz:

1. `gcc` invoca o compilador C (GNU Compiler Collection).
2. `echo.c` é o arquivo-fonte a ser compilado.
3. `-o echo` diz ao compilador para nomear o executável gerado como `echo` (sem essa flag, o padrão seria `a.out`).

Ao final, surge na pasta um arquivo binário executável chamado `echo`.

---

## 3. O programa `shell.c`

### 3.1 Do pseudocódigo ao código real

O ponto de partida foi este pseudocódigo (comum em livros de Sistemas Operacionais, como o de Tanenbaum):

```c
while (TRUE) { 
    type_prompt(); 
    read_command(command, parameters); 
    if (fork() != 0) { /* Parent code */ 
        waitpid(-1, &status, 0); 
    } else { /* Child code */ 
        execve(command, parameters, 0); 
    } 
}
```

Ele descreve a **lógica** de um shell, mas `type_prompt()` e `read_command()` são só nomes de função — não existem de verdade em C. Para funcionar, é preciso implementá-los.

### 3.2 O código completo

```c
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
            perror("execve falhou");
            exit(1);
        }
    }
    return 0;
}
```

### 3.3 Explicando cada função

#### `type_prompt()`

```c
void type_prompt() {
    printf("minishell> ");
    fflush(stdout);
}
```

- Imprime o prompt (`minishell> `) para o usuário saber que o shell está esperando um comando.
- `fflush(stdout)` força a saída a aparecer imediatamente na tela. Sem isso, o texto poderia ficar "preso" no buffer e só aparecer depois, causando confusão visual.

#### `read_command(command, parameters)`

Esta função lê o que o usuário digitou e transforma em algo que o `execve` entenda.

```c
char line[MAX_LINE];
fgets(line, MAX_LINE, stdin);
line[strcspn(line, "\n")] = '\0';
```
- `fgets` lê uma linha inteira digitada pelo usuário (até `MAX_LINE` caracteres) e guarda em `line`.
- `fgets` inclui o `\n` (Enter) no final da string; a linha seguinte remove esse `\n`, substituindo-o por `\0` (fim de string).

```c
int i = 0;
char *token = strtok(line, " ");
while (token != NULL) {
    parameters[i] = token;
    i++;
    token = strtok(NULL, " ");
}
parameters[i] = NULL;
```
- `strtok` quebra a string em "pedaços" (*tokens*) separados por espaço. Por exemplo, `"./echo ola mundo"` vira três tokens: `"./echo"`, `"ola"`, `"mundo"`.
- Cada token é guardado em `parameters[i]`.
- Ao final, `parameters[i] = NULL` marca o fim do vetor — isso é **obrigatório** para o `execve` funcionar, pois ele não sabe o tamanho do vetor, só olha até encontrar `NULL`.

```c
strcpy(command, parameters[0]);
```
- Copia o primeiro token (o nome/caminho do programa) para a variável `command`, que será usada como primeiro argumento do `execve`.

#### `main()` — o coração do shell

```c
while (TRUE) {
    type_prompt();
    read_command(command, parameters);

    if (fork() != 0) {
        waitpid(-1, &status, 0);
    } else {
        execve(command, parameters, NULL);
        perror("execve falhou");
        exit(1);
    }
}
```

Este laço roda **para sempre**, repetindo o ciclo: mostrar prompt → ler comando → executar.

---

## 4. O que acontece na memória: `fork()` e `execve()`

Esta é a parte mais importante para entender **como o sistema operacional realmente executa um comando**.

### 4.1 `fork()`: duplicando o processo

```c
if (fork() != 0) {
    /* código do pai */
} else {
    /* código do filho */
}
```

- `fork()` cria uma **cópia exata** do processo atual (o shell). Agora existem dois processos rodando o mesmo código, com o mesmo estado de memória.
- A diferença é o **valor de retorno** de `fork()`:
  - No processo **pai**, `fork()` retorna o PID (número de identificação) do filho — um valor **diferente de zero**. Por isso o `if (fork() != 0)` entra no bloco do pai.
  - No processo **filho**, `fork()` retorna **0**. Por isso ele cai no `else`.

É como se, no exato momento do `fork()`, o programa "se dividisse em dois", cada cópia seguindo por um caminho diferente do `if/else`.

### 4.2 `execve()`: transformando o filho em outro programa

```c
execve(command, parameters, NULL);
```

- `execve` **substitui completamente** o código e a memória do processo filho pelo código de outro programa — no nosso caso, o executável `echo`.
- Os três argumentos:
  1. `command`: caminho do executável a rodar (ex: `"./echo"`).
  2. `parameters`: o vetor de argumentos, que vira o `argv` do novo programa (`argv[0], argv[1], ...`, terminado em `NULL`).
  3. `NULL` (ou `envp`): variáveis de ambiente a passar ao novo programa. Usar `NULL` significa que o programa não recebe variáveis de ambiente do sistema.
- Se `execve` funcionar, o processo filho **nunca mais volta a executar o código do shell** — ele virou o programa `echo` de corpo e alma, começando do `main(argc, argv)` do `echo.c`.
- Se `execve` **falhar** (por exemplo, caminho errado), ele retorna `-1` e o código continua na linha seguinte — por isso colocamos `perror` e `exit(1)`: para avisar o erro e encerrar o filho (senão ele continuaria rodando como uma cópia do shell, o que causaria bagunça).

### 4.3 `waitpid()`: o pai espera o filho terminar

```c
waitpid(-1, &status, 0);
```

- O processo **pai** (o shell) chama `waitpid` para **bloquear** sua própria execução até que o filho termine.
- `-1` significa "espere por qualquer filho".
- `&status` guarda informações sobre como o filho terminou (código de saída, se foi encerrado por sinal, etc.).
- Sem essa espera, o shell voltaria a mostrar o prompt (`minishell> `) **antes** do `echo` terminar de imprimir, misturando as saídas na tela.

### 4.4 Diagrama do fluxo completo

```
[Shell rodando]
      |
      | usuário digita: ./echo ola mundo
      v
[read_command monta command="./echo", parameters=["./echo","ola","mundo",NULL]]
      |
      v
   fork()
      |
   +--+-------------------+
   |                       |
 PAI (shell)          FILHO (cópia do shell)
   |                       |
 waitpid()             execve("./echo", parameters, NULL)
   |  (bloqueado)          |
   |                  processo filho "vira" o programa echo
   |                       |
   |                  main(argc=3, argv) do echo.c roda
   |                       |
   |                  imprime "ola mundo\n"
   |                       |
   |                  return 0  (o processo termina)
   |                       |
   |<---- filho morre -----+
   |
 waitpid() desbloqueia
   |
   v
[Shell volta ao topo do while, mostra o prompt de novo]
```

---

## 5. Compilando e executando na prática

### 5.1 Compilar os dois programas

```bash
gcc echo.c -o echo
gcc shell.c -o shell
```

Cada comando gera um executável binário (`echo` e `shell`) na pasta atual.

### 5.2 Verificar se os arquivos foram criados

```bash
ls
```

Deve aparecer algo como:
```
echo  echo.c  shell  shell.c
```

### 5.3 Rodar o shell

```bash
./shell
```

O terminal deve mostrar o prompt:
```
minishell>
```

### 5.4 Executar o `echo` a partir do mini-shell

```
minishell> ./echo ola mundo
ola mundo
minishell>
```

O que acontece nos bastidores, em ordem:
1. `shell` está rodando e mostra `minishell> `.
2. Você digita `./echo ola mundo` e aperta Enter.
3. `read_command` separa isso em `command = "./echo"` e `parameters = ["./echo", "ola", "mundo", NULL]`.
4. `fork()` cria um processo filho.
5. O filho chama `execve("./echo", parameters, NULL)`, que carrega o binário `echo` na memória do filho.
6. O `main` do `echo.c` roda com `argc=3` e o `argv` correspondente, imprimindo `ola mundo`.
7. O `echo` termina (`return 0`), o que encerra o processo filho.
8. O `waitpid` do pai (shell) desbloqueia.
9. O laço `while` volta ao início, mostrando o prompt novamente.

### 5.5 Por que é preciso digitar `./echo` e não só `echo`

`execve` **não pesquisa a variável de ambiente `PATH`** — diferente de comandos digitados diretamente no terminal do sistema (bash), que sabe procurar executáveis em pastas como `/usr/bin`. Por isso:

- `./echo` funciona: é um caminho relativo, apontando exatamente para o executável na pasta atual.
- `echo` sozinho **falharia**, porque o `execve` tentaria abrir um arquivo chamado literalmente `echo` sem saber em qual pasta procurar, resultando no erro `ENOENT` (arquivo não encontrado) — que o `perror("execve falhou")` exibiria na tela.

---

## 6. Resumo dos conceitos-chave

| Conceito | O que é |
|---|---|
| `argc` / `argv` | Forma como o `main` recebe os argumentos passados na linha de comando |
| `fork()` | Cria uma cópia do processo atual; retorna 0 no filho, PID do filho no pai |
| `execve()` | Substitui o código do processo atual pelo código de outro programa |
| `waitpid()` | Faz o processo pai esperar o filho terminar antes de continuar |
| `strtok()` | Quebra uma string em tokens separados por um delimitador (aqui, espaço) |
| `PATH` | Lista de pastas onde o sistema procura executáveis — **não usada** pelo `execve` puro |

Esse padrão (`fork` + `execve` + `wait`) é exatamente como shells reais, como o `bash`, executam comandos digitados pelo usuário — só que com bem mais tratamento de erros, redirecionamento de entrada/saída, pipes, variáveis de ambiente, etc.

/usr/bin/ls listar os diretórios da pasta
/usr/bin/cat echo.c listar o código passado