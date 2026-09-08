# Sensor IR de presença — Goertzel + lock-in (STM32G4)

Firmware do sensor de presença próprio para o robô de sumo. Cada sensor tem
seu micro; este repositório é o código **de um** sensor.

Implementa a cascata de decisão do documento de projeto do eletricista
("TUTORIAL INSANO DE COMO IMPLEMENTAR O GOERTZEL + LOCK-IN AMPLIFICATION"):

```
ETAPA A   média CC do bloco > limiar?      ->  CEGAMENTO / INIMIGO COLADO
ETAPA B   Goertzel em 38 e 56 kHz?         ->  INIMIGO (via emissão dele)
                                               furtividade mantida: não emitimos
ETAPA C   emissor 90 kHz + lock-in ±1      ->  INIMIGO (via sinal próprio)
          senão                            ->  VAZIO
```

## Estado

| | |
|---|---|
| Core de DSP + máquina de estados | pronto e testado no PC |
| Adapters do STM32G4 (PWM, ADC+DMA, OPAMP/PGA) | escritos, **não compilados contra HAL real** |
| `board_config.h` | precisa ser conferido contra o `.ioc` |
| Adapter de I²C escravo | não escrito (depende do barramento do robô) |
| Validação em hardware | não feita |

O `.ioc` do CubeMX não chegou na sessão em que isto foi escrito, então a
plataforma foi isolada num único arquivo (`board_config.h`) com tudo o que
depende dele marcado `[IOC]`. Os `_Static_assert` desse arquivo travam o
build se algo divergir do plano de frequências.

Testes: **20146 checagens no DSP, 52 na cascata, 0 falhas.**

```bash
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

## O plano de frequências

```
TIMCLK = 144 MHz    Fs = 300 kSPS    N = 300    df = 1 kHz    bloco = 1 ms
f0 = 90 kHz (bin 90)                 ciclo = 4 blocos = 4 ms -> 250 decisões/s
```

Com `df = 1 kHz` exato, **toda** frequência de interesse (30/33/36/38/40/56/90
kHz) cai em bin inteiro: perda de scalloping zero. E `144e6/90e3 = 1600` e
`144e6/300e3 = 480` são inteiros exatos, então portadora e ADC saem do mesmo
clock sem deriva.

Duas decisões que valem a leitura de `docs/DSP.md`:

- **Fs = 300 kSPS e não 360 kSPS (= 4·f₀).** O truque de amostrar em 4·f₀
  torna o misturador de quadratura puro somas, mas com PWM de 10% a 3ª
  harmônica (−1.2 dBc, quase tão forte quanto o fundamental) dobraria
  **exatamente em cima da portadora**. Em 300 kSPS ela cai em 30 kHz e só a
  9ª/11ª (−19 dBc, vindas de 810/990 kHz) voltam, onde o anti-aliasing as
  mata.
- **Goertzel como default, ±1 disponível.** Medido em `test_dsp`: a
  referência ±1 vaza a 3ª harmônica a −8.4 dB; a senoidal, a −283 dB. Num
  M4F a 144 MHz o Goertzel custa 0.5% de CPU, então não há motivo para
  aceitar o vazamento. A variante ±1 do plano está implementada e testada
  (`ir_sensor_set_square_ref`).

## Resultados medidos (simulador)

```
rejeição de CC                  77.9 dB
rejeição de ripple de rede      56.8 dB
rejeição de 38 kHz              83.5 dB
rejeição de 56 kHz              97.5 dB
rejeição do bin adjacente       73.2 dB

cancelamento de crosstalk       ~400x  (39990 -> 97)
imunidade a luz ambiente        0.1%   de variação na medida
imunidade a jammer em 90 kHz    0.1%   (chopping), e levanta IR_FLAG_JAMMED
furtividade sob ameaça          0      blocos com emissor ligado
alcance                         ~40-50 cm (alvo branco, refletividade 0.8)
ganho de processamento          ~39 dB (150 kHz de banda -> 16 Hz)
```

O alcance depende inteiramente da óptica real — o número acima vem do modelo
do simulador, não de bancada.

## ⚠️ Antes de ligar na placa

Três itens de hardware são **bloqueantes**. Sem eles o firmware está certo e
o sensor não funciona. Detalhes em `docs/HARDWARE.md`:

1. **Fototransistor não serve.** Em 90 kHz está ≥15 dB abaixo. Precisa de
   **fotodiodo PIN** (BPW34/SFH203P) polarizado reverso.
2. **Filtro anti-aliasing de 2 polos em ~130 kHz.** O OPAMP em high-speed
   (13 MHz de GBW) não rola sozinho; sem o filtro, harmônicas e ruído de
   banda larga dobram para dentro.
3. **Modo high-speed no OPAMP, ganho máximo 32.** Em modo normal (1.6 MHz de
   GBW) nem o ganho 16 fecha a banda necessária. O plano já pedia isso; a
   conta confirma.

## Documentação

| | |
|---|---|
| `docs/DSP.md` | de onde vem cada número, aliasing de harmônicas, coerência entre blocos, orçamento de CPU, headroom de ponto fixo, FDMA |
| `docs/HARDWARE.md` | requisitos do front-end, topologia com os 2 OPAMPs, checklist de bancada |
| `docs/ARQUITETURA.md` | camadas, caminho de tempo real, como integrar ao CubeMX, interface com o micro principal |

## Ferramenta de bancada

O core roda no PC contra um modelo do front-end, o que permite afinar
limiares antes de a placa existir:

```bash
./build/sweep dist     # varre distância do alvo        -> CSV
./build/sweep threat   # varre amplitude do adversário  -> CSV
./build/sweep noise    # varre ruído do front-end       -> CSV
./build/sweep trace    # série temporal de um cenário de luta
```

Os limiares de `firmware/core/config/sensor_config.h`
(`CFG_PRESENCE_SNR_ON_Q8` etc.) devem ser escolhidos olhando estas curvas com
os números da **sua** óptica.

## Vários sensores no mesmo robô

Sensores vizinhos se ouvem. Dê um bin diferente a cada um — todos exatos a
partir de 144 MHz e em bin inteiro, o que põe o vazamento mútuo num zero
exato da janela (~−69 dB com cristais reais):

```
bin  72 -> 72 kHz      bin  96 ->  96 kHz
bin  80 -> 80 kHz      bin 100 -> 100 kHz
bin  90 -> 90 kHz      bin 120 -> 120 kHz
```

Compile cada um com `-DCFG_CARRIER_BIN=<bin>`.

## Estrutura

Ports & adapters: o core (`firmware/core/`) inclui **apenas**
`firmware/port/ports.h` — nunca HAL, nunca `board_config.h`. É isso que faz o
mesmo `ir_sensor.c` que passa nos testes ser o que vai para a placa, e que
torna a troca de plataforma um trabalho de dois arquivos.

Esta estrutura é uma **proposta**: como a arquitetura do projeto principal
não chegou, escolhi o padrão que sobrevive melhor num sensor com micro
próprio. Quando você mandar a do projeto principal, a adaptação é mecânica
(renomear/mover camadas) — nenhuma lógica muda, porque o core não conhece
nenhuma delas.
