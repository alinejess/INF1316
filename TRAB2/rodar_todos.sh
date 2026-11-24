#!/usr/bin/env bash
# Roda as 32 simulações (4 logs x 4 algoritmos x 2 páginas x 2 memórias)
# e gera a tabela em tabela.csv e tabela.md.
# Uso: ./rodar_todos.sh   (com os .log na mesma pasta)
set -e

gcc -std=c11 -O2 -Wall -Wextra -o sim-virtual sim-virtual.c

echo "programa,algoritmo,pagina_KB,memoria_MB,page_faults,paginas_escritas" > tabela.csv
{
  echo "| Programa | Algoritmo | Página (KB) | Memória (MB) | Page faults | Páginas sujas escritas |"
  echo "|---|---|---|---|---|---|"
} > tabela.md

for log in compilador.log matrix.log compressor.log simulador.log; do
  if [ ! -f "$log" ]; then
    echo "AVISO: $log não encontrado, pulando." >&2
    continue
  fi
  for alg in LRU NRU FIFO Otimo; do
    for mem in 2 4; do
      for pag in 4 8; do
        saida=$(./sim-virtual "$alg" "$log" "$pag" "$mem")
        faltas=$(echo "$saida" | awk -F': ' '/Faltas/{print $2}')
        escritas=$(echo "$saida" | awk -F': ' '/Escritas/{print $2}')
        echo "${log%.log},$alg,$pag,$mem,$faltas,$escritas" >> tabela.csv
        echo "| ${log%.log} | $alg | $pag | $mem | $faltas | $escritas |" >> tabela.md
        echo "${log%.log} $alg pag=$pag mem=$mem -> faltas=$faltas escritas=$escritas"
      done
    done
  done
done

echo
echo "Tabela gerada em tabela.csv e tabela.md"
