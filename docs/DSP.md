# O DSP: de onde vêm os números

Este documento existe para que ninguém precise chutar nada em
`sensor_config.h`. Cada constante tem uma derivação.

## 1. A correção que muda o projeto

O plano fala em "emitir num comprimento de onda onde os sensores padrões não
enxergam". Vale separar dois eixos que costumam ser confundidos, porque só um
deles é firmware:

| Eixo | O que é | Onde se resolve |
|---|---|---|
| **Comprimento de onda** (λ) | 850 nm vs 940 nm — física do LED e do filtro óptico | **Hardware.** Nenhum algoritmo muda isso |
| **Frequência da portadora** (f₀) | 38 kHz vs 90 kHz — a modulação | **Firmware.** É aqui que Goertzel e lock-in atuam |

Goertzel e lock-in são seletivos **em frequência de modulação**, não em
comprimento de onda.

A boa notícia é que o eixo controlável por firmware é justamente o que dá
furtividade de verdade. Trocar λ de 940 para 850 nm rende pouco: fotodiodos
são banda-larga, e um receptor otimizado para 940 nm ainda vê 850 nm com
60–90% de responsividade. Já a portadora rende muito, por dois motivos
independentes:

1. **Banda passante.** Um TSOP/TSSP tem um filtro estreito em torno da
   frequência nominal. Em 90 kHz ele não tem ganho nenhum.
2. **AGC.** Esses receptores *suprimem ativamente* portadora contínua — o AGC
   interno a trata como ruído ambiente em algumas centenas de µs. Isto é
   quase mais importante que o item 1: mesmo que algo vazasse pela banda, o
   AGC mata.

Ou seja: portadora **contínua**, em **frequência fora da grade padrão** é
quase invisível para o adversário — e é exatamente o sinal ideal para
lock-in. Os dois objetivos apontam para o mesmo lado. Os 90 kHz do plano
estão certos.

## 2. O plano de amostragem

```
TIMCLK = 144 MHz
Fs     = 300 kSPS      ARR do timer do ADC       = 144e6/300e3 =  480   exato
f0     =  90 kHz       ARR do timer da portadora = 144e6/90e3  = 1600   exato
N      = 300 amostras  bloco = 1.000 ms          df = Fs/N     = 1000 Hz
Nyquist = 150 kHz
```

O achado que amarra tudo: com **df = 1 kHz exato**, toda frequência de
interesse cai em **bin inteiro**.

| Alvo | bin ideal | bin usado | erro | perda de scalloping |
|---|---|---|---|---|
| 30 kHz | 30.000 | 30 | 0 Hz | 0.00 dB |
| 33 kHz | 33.000 | 33 | 0 Hz | 0.00 dB |
| 36 kHz | 36.000 | 36 | 0 Hz | 0.00 dB |
| **38 kHz** | 38.000 | 38 | 0 Hz | 0.00 dB |
| 40 kHz | 40.000 | 40 | 0 Hz | 0.00 dB |
| **56 kHz** | 56.000 | 56 | 0 Hz | 0.00 dB |
| **90 kHz** | 90.000 | 90 | 0 Hz | 0.00 dB |

Perda de scalloping zero significa que a janela retangular está exatamente
casada com cada tom: nenhuma amplitude é subestimada, nenhum limiar precisa
de fator de correção.

## 3. Aliasing de harmônicas: por que Fs = 300k e não 360k

Este é o ponto onde a escolha "natural" está errada.

O truque clássico é amostrar em **Fs = 4·f₀** (360 kSPS), porque aí a
referência de quadratura vira `[+1, 0, -1, 0]` e o misturador não precisa de
multiplicação nenhuma. Tentador — mas com PWM de 10% é uma armadilha.

Um trem de pulsos de duty *d* tem harmônica *n* com amplitude
`|sin(nπd)|/(nπd)` relativa ao fundamental. Em d = 10%:

| harmônica | amplitude relativa |
|---|---|
| 3ª | **−1.2 dBc** |
| 5ª | −3.8 dBc |
| 7ª | −8.5 dBc |

A 3ª harmônica tem praticamente a **mesma amplitude do fundamental**. E em
Fs = 4·f₀, *todas* as harmônicas ímpares dobram exatamente em cima do bin da
portadora — corrompendo a medida de forma irrecuperável.

Em **Fs = 300 kSPS** (= 10/3 · f₀) o quadro é outro:

| harmônica | frequência | dobra em | cai na portadora? |
|---|---|---|---|
| 3ª (−1.2 dBc) | 270 kHz | 30 kHz (bin 30) | **não** |
| 5ª (−3.8 dBc) | 450 kHz | 150 kHz (Nyquist) | não |
| 7ª (−8.5 dBc) | 630 kHz | 30 kHz | não |
| 9ª (−19 dBc) | 810 kHz | 90 kHz | sim, mas −19 dBc e filtrada |
| 11ª (−21 dBc) | 990 kHz | 90 kHz | sim, mas −21 dBc e filtrada |

Só a 9ª e a 11ª voltam para a portadora, a −19 dBc, vindas de 810 kHz e
990 kHz — onde qualquer filtro anti-aliasing razoável as elimina.

**Consequência de projeto:** a 3ª harmônica dobra em 30 kHz. Por isso o bin
30 está **fora** de `CFG_THREAT_BINS`. Se um dia for preciso vigiar 30 kHz,
só é confiável em bloco com o emissor desligado.

**Custo dessa escolha:** perde-se o misturador de somas puras e usa-se
Goertzel (1 multiplicação + 2 somas por amostra). Num Cortex-M4F a 144 MHz
isso é ~0.5% de CPU. Foi barato.

## 4. Coerência entre blocos: a condição que sustenta o ganho

Somar I/Q de blocos diferentes só é válido se a fase da portadora for a mesma
no início de cada bloco. Duas condições, ambas verificadas por
`_Static_assert`:

1. **Sem deriva.** Os dois timers saem do mesmo clock de 144 MHz com ARR
   inteiros (1600 e 480). Não existe deriva relativa — nunca.
2. **Fase se repete.** `N·f₀/Fs = 300·90000/300000 = 90` ciclos **inteiros**
   por bloco. O bloco termina na mesma fase em que começou.

Se qualquer uma quebrar, a EWMA sobre I/Q passa a somar fasores girando e a
magnitude despenca — sem erro nenhum aparecer. É o tipo de falha que só
aparece em bancada com um osciloscópio e três dias perdidos, então o build
trava antes.

A defasagem *inicial* entre os dois timers (poucos ciclos, na partida) é
irrelevante: é constante, e a calibração de crosstalk a absorve.

## 5. Ganho de processamento

```
Goertzel de 1 bloco:  ENBW = Fs/N = 1000 Hz
EWMA (shift=2):       tau = 4 ciclos x 4 ms = 16 ms  ->  ENBW ~ 16 Hz
```

De ~150 kHz de banda analógica para ~16 Hz de banda de detecção:
**~39 dB de ganho de SNR**. É esse número que transforma um reflexo fraco a
40 cm em detecção confiável.

O ganho mora no **filtro pós-detecção**, não no Goertzel. É por isso que
filtrar **I e Q separadamente** (e não a magnitude) é essencial: ruído de
fase aleatória cancela, alvo de fase fixa soma. Filtrar magnitude jogaria
essa vantagem no lixo.

## 6. Goertzel vs lock-in ±1 — medido, não opinado

Matematicamente são a mesma correlação. A diferença prática é a forma da
referência, e `test_dsp` mede:

```
vazamento da 3a harmonica no bin da portadora:
  referencia +-1      :   8.4 dB abaixo do fundamental
  referencia senoidal : 283.5 dB abaixo do fundamental
```

A referência quadrada correlaciona com todas as harmônicas ímpares (peso
1/3, 1/5, …). Com PWM de 10% — rico em harmônicas — isso importa.

Por isso o **default é o Goertzel** (referência senoidal) e a variante ±1 do
plano fica disponível em `ir_sensor_set_square_ref(true)`, implementada e
testada, para o caso de o firmware migrar para um micro sem multiplicador.
Numa comparação de cadeia completa as duas detectam alvo a 30 cm; a ±1 dá
magnitude um pouco maior justamente porque soma harmônicas.

Detalhe da implementação ±1: a referência tem período `N/gcd(bin,N)` = 10
amostras (não `Fs/f₀` = 10/3, que não é inteiro), e é de **3 níveis**
(−1/0/+1) porque `cos` e `sin` passam por zero exato. Três níveis rejeitam a
3ª harmônica bem melhor que onda quadrada pura.

## 7. Orçamento de CPU (Cortex-M4F @ 144 MHz)

| Tarefa | Custo |
|---|---|
| Goertzel da portadora, todo bloco | 300 MAC/ms = 300 kMAC/s |
| Varredura de ameaça, 5 bins + ruído, nos blocos de escuta | ~450 kMAC/s |
| Centrar bloco + saturação | 300 op/ms |
| Pós-detecção (sqrt, atan2, EWMA) | 1×/ciclo de 4 ms |
| **Total** | **< 1 MMAC/s, ~0.5% de CPU** |

O DSP roda no laço principal, não no ISR: o ISR só etiqueta o bloco, avança
a fase e chaveia o emissor. Sobram 999 µs de folga por bloco. Se o laço
atrasar, o core conta overrun e levanta `IR_FLAG_BLOCK_OVERRUN` — a falha
aparece no relatório em vez de virar medida errada em silêncio.

## 8. Headroom de ponto fixo

O ressonador de Goertzel tem polos **sobre** o círculo unitário: é
marginalmente estável e `|s|` cresce linearmente quando o sinal está no bin.

```
|s|max ~= (N/2) * A = 150 * 2048 = 307200 ~= 2^18.2
coeff em Q14        <= 32768        = 2^15
produto coeff*s1    <= 2^33          -> NAO cabe em int32
```

Por isso o produto é `int64`. Não é concessão: em Cortex-M4 é `SMULL`,
1 ciclo. Em troca, Q14 dá **8× mais precisão de fase** que Q11 — e a fase é
o que sustenta a subtração vetorial do crosstalk. O efeito é medido:

| | rejeição em 38 kHz | bin adjacente |
|---|---|---|
| Q11 | 77.9 dB | 49.8 dB |
| Q14 | **83.5 dB** | **73.2 dB** |

Para migrar a um Cortex-M0 sem multiplicador de 64 bits: baixe `COEFF_Q`
para 11 em `tools/gen_dsp_tables.py`; o produto passa a caber em int32 com
1 bit de margem, ao custo de ~0.01 rad de erro de fase.

## 9. FDMA entre os nossos próprios sensores

Cada sensor tem seu micro, e vizinhos apontando em direções próximas vão se
ouvir. A solução é dar um bin diferente a cada um. Todos exatos a partir de
144 MHz e todos em bin inteiro:

| bin | f₀ | ARR |
|---|---|---|
| 72 | 72 kHz | 2000 |
| 80 | 80 kHz | 1800 |
| **90** | **90 kHz** | **1600** |
| 96 | 96 kHz | 1500 |
| 100 | 100 kHz | 1440 |
| 120 | 120 kHz | 1200 |

Compile cada sensor com `-DCFG_CARRIER_BIN=<bin>`.

O ponto bonito: como a janela é retangular e todos os canais estão em bins
**inteiros**, o vazamento de um vizinho no nosso bin cai num **zero exato**
da janela. Com cristais reais (±20 ppm ⇒ ±1.8 Hz em 90 kHz ⇒ 0.002 bin de
desvio) o vazamento fica em torno de −69 dB. E o que sobrar disso ainda passa
pelo chopping, que cancela interferência estática na nossa frequência.

## 10. Outros clocks

170 MHz (o máximo do G4) **não serve**: `170e6/90e3 = 1888.9`, não inteiro.
Rodar a 170 MHz obriga a refazer o plano inteiro. Se for necessário, o
procedimento é: escolher `P_c` e `P_s` inteiros tais que `f₀ = TIMCLK/P_c`,
`Fs = TIMCLK/P_s`, com `N·P_s/P_c` inteiro, e reverificar a tabela de
aliasing de harmônicas da seção 3. Depois rodar
`tools/gen_dsp_tables.py --n <N> --fs <Fs>`.
