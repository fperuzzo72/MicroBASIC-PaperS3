# Programas de exemplo

Programas BASIC prontos para o MicroBASIC. Todos foram rodados no harness de
host (`scratchpad/tbrepl`, ver `docs/DEVELOPMENT_LOG.md`) antes de entrar
aqui — a lógica de cada um chegou ao fim, com vitória e derrota, sem erro.

## Como pôr no aparelho

Pelo menu **Sync**, aba **BASIC programs**, envie o `.bas`. Ele cai em
`/MicroBASIC/programs`, que é exatamente onde o `LOAD` procura. Depois:

```
LOAD "invaders.bas"
RUN
```

Ou use `VC`, que lista a pasta e carrega o escolhido.

## O que tem

| arquivo | SCREEN | tipo | o que exercita |
|---|---|---|---|
| `pacman.bas` | 1 (48 col) | tempo real | `GET`, `LOCATE`, substring como buffer, `@T`, `RND` |
| `invaders.bas` | 2 (64 col) | tempo real | o mesmo, mais construção de string por posição |
| `lander.bas` | qualquer | por turnos | `INPUT` numérico, aritmética de ponto flutuante |
| `forca.bas` | qualquer | por turnos | `INPUT` de string, `READ`/`DATA`, `ASC`, substring |
| `menu.bas` | qualquer | lançador | `LOAD` encadeado, `GET`, seleção com setas |
| `sokoban.bas` | 1 ou maior | por turnos | duas camadas em strings, `GET` bloqueante, 4 fases em `DATA` |

## Veredito do hardware

Testado no aparelho: os dois jogos em tempo real ficaram **toleráveis, não
bons**. A um quadro por segundo essa é a resposta honesta, e nenhuma
esperteza no programa muda isso — o painel é o painel.

O que isso indica é o gênero, não a implementação. O alvo certo é o
quebra-cabeça no espírito do ZX81: empurrar caixas, problemas em que a
*ordem* das jogadas decide se tem solução. Ali o segundo entre quadros não é
espera, é o tempo de olhar o tabuleiro. `lander.bas`, `forca.bas` e sobretudo
`sokoban.bas` já funcionam assim: o `sokoban` só repinta quando você aperta
uma tecla, então não existe quadro perdido — o painel fica parado enquanto
você pensa, que é exatamente o que se quer dele.

## Uma nota sobre e-ink e ritmo

O painel leva ~700ms por refresh, e o runtime espalha isso num intervalo de
~400ms de execução entre repinturas. Na prática **um quadro leva cerca de um
segundo**. Isso não é um defeito a contornar, é o material com que se está
trabalhando, e cada programa aqui foi desenhado em torno disso:

- **Por turnos ganha de tempo real.** `lander.bas` e `forca.bas` são os que
  melhor funcionam, porque cada quadro corresponde a uma decisão sua. Um
  segundo de espera depois de você digitar algo não é lentidão, é pausa.
- **Em tempo real, comande a intenção, não o movimento.** No `pacman.bas` a
  seta escolhe uma direção e ele segue nela até a parede; você não empurra o
  boneco a cada passo. Assim a espera pelo painel não é uma espera *sua*.
- **Nada de projétil viajando.** No `invaders.bas` o tiro é instantâneo:
  atinge o invasor mais baixo da coluna sob a nave. Um projétil subindo uma
  célula por quadro levaria dez segundos para chegar ao topo. A adaptação é
  explícita, não um atalho.
- **Repinte só o que mudou.** Todos usam `LOCATE` para reescrever células
  individuais. Redesenhar a tela inteira a cada quadro custaria vários
  refreshes.

## O lançador, e o autoexec

`menu.bas` é um menu: setas escolhem, ENTER carrega e roda. A primeira opção
apenas termina, caindo no prompt do BASIC.

Ele funciona porque **`LOAD` encadeia**: chamado de dentro de um programa em
execução, zera o programa atual, carrega o novo e o roda. Nada precisou ser
inventado para isso.

Gravado no cartão com o nome **`autoexec.bas`**, o interpretador o encontra
sozinho no arranque e o executa — o aparelho liga direto no menu. Se algo
der errado com o programa de arranque, **segurar BACK no boot pula o
autoexec**. Essa saída existe porque a falta dela custou uma recuperação por
cabo e esptool: um lançador roda para sempre, e enquanto um programa roda o
`loop()` do firmware está parado.

A lista de programas é fixa, em `DATA`. Um programa BASIC não consegue listar
o diretório: o runtime tem `rootopen`/`rootnextfile`, mas nada disso está
exposto à linguagem. Pelo mesmo motivo o lançador não pode chamar o `VC` nem
o `SCREEN` — os dois são comandos do firmware, interceptados antes do
interpretador, e só existem digitados no prompt.

## Detecção de resolução

Nenhum programa consegue perguntar ao sistema em que `SCREEN` está. O
comando `SCREEN` é do firmware, não do interpretador, e as variáveis `@X`/`@Y`
do interpretador (que dariam o tamanho do display) estão atrás de um
`DISPLAYDRIVER` que este firmware não define.

Por isso `invaders.bas` **pergunta** no início e sai se a resposta for não.
O `sokoban.bas` precisa de 13 linhas, então não cabe no SCREEN 0 (que tem
10); em SCREEN 1 ou maior fica bem. Ele desenha o tabuleiro nas colunas 18 a
29 e a legenda dos símbolos nas colunas 2 a 11 — as duas faixas não se
encostam em nenhum modo a partir do SCREEN 1.

### As 4 fases do sokoban

Verificadas por um solver BFS antes de entrarem no arquivo, com a solução
ótima em movimentos: **11, 17, 25 e 28**. Dois candidatos que pareciam bons
no papel eram insolúveis e foram descartados — um nível de Sokoban impossível
não se anuncia, então a verificação não era opcional.
`pacman.bas` só avisa num `REM`. Se um dia o `SCREEN` virar um token do
interpretador (ver `Next up` no README principal), os dois podem simplesmente
se ajustar sozinhos.
