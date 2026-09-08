# Arquitetura

## Aviso: esta estrutura é uma proposta, não a definitiva

O `.ioc` do CubeMX **não chegou** nesta sessão (o diretório de anexos do
container estava vazio), e o repositório estava sem nenhum commit — portanto
não vi a arquitetura base do projeto principal.

O que está aqui é **ports & adapters** (hexagonal), que é o padrão que
sobrevive melhor num sensor com micro próprio. Quando você mandar a
arquitetura do projeto principal, a adaptação é mecânica: renomear/mover
camadas. Nada de lógica muda, porque o core não conhece nenhuma delas.

## A regra única

```
firmware/core/  ->  inclui APENAS firmware/port/ports.h
                    nunca stm32*_hal.h, nunca board_config.h
```

É isso que faz o core rodar **idêntico** no PC e no G4 — o mesmo
`ir_sensor.c` que passa nos 52 testes é o que vai para a placa. Se algum dia
o core precisar de um include de HAL para compilar, a arquitetura foi
violada.

O CMake do host compila `core/` e `app/` sem nenhum stub de HAL. Isso não é
enfeite: é o teste automático de que a regra continua valendo.

## Camadas

```
firmware/
├── core/                        plataforma-independente, C11, sem libm, sem FPU
│   ├── config/sensor_config.h   TODOS os números do DSP e limiares
│   ├── dsp/
│   │   ├── fx_math.{h,c}        ponto fixo: isqrt64, atan2, EWMA
│   │   ├── fx_tables.h          GERADO por tools/gen_dsp_tables.py
│   │   ├── goertzel.{h,c}       DFT de 1 bin (o misturador)
│   │   └── lockin.{h,c}         chopping + crosstalk + LPF I/Q, e a variante ±1
│   └── sensor/
│       ├── ir_sensor.{h,c}      a cascata A -> B -> C do plano
│       ├── threat_scan.{h,c}    ETAPA B: escuta passiva 38/56 kHz
│       └── hysteresis.{h,c}     histerese + votação M-de-N
│
├── port/ports.h                 os contratos (emitter, pga, time, log)
│
├── platform/
│   ├── stm32/
│   │   ├── board_config.h       >>> O ÚNICO ARQUIVO QUE O .ioc AFETA <<<
│   │   └── stm32_adapters.{h,c} a única parte que fala HAL
│   └── host/
│       └── sim_frontend.{h,c}   modelo do front-end + cena, para rodar no PC
│
├── app/
│   ├── app.{h,c}                relatório para o micro principal
│   ├── main_stm32.c             composition root do G4
│   └── main_host.c              ferramenta de bancada (varreduras em CSV)
│
└── test/
    ├── test_dsp.c               DSP contra DFT de força bruta em double
    └── test_sensor.c            cascata completa contra o simulador
```

## Caminho de tempo real

O ponto mais delicado do desenho. Duas coisas com requisitos opostos:

- **chavear o emissor** tem de acontecer *exatamente* na fronteira do bloco
  (se atrasar, o bloco ON_MEASURE pega parte de bloco com LED apagado e o
  chopping mede errado);
- **o DSP** não precisa de urgência nenhuma — tem 1 ms de folga.

Solução: separar os dois.

```
ISR do DMA (metade/completa)          laço principal
──────────────────────────            ──────────────
ir_sensor_on_block_isr()              ir_sensor_process()
  etiqueta o bloco com a fase           drena a fila
  enfileira (2 slots)                   centra o bloco, roda Goertzel
  avança a fase                         chopping, crosstalk, EWMA
  chaveia o emissor  <-- barato         decide o veredito
```

A fila tem 2 slots, casando com o bloco duplo do DMA (circular sobre
2×300 amostras, meia-transferência + transferência-completa). O core sempre
processa um bloco que o DMA não está escrevendo.

Se o laço principal atrasar, o core **conta o overrun e levanta
`IR_FLAG_BLOCK_OVERRUN`**. A falha aparece no relatório — nunca vira medida
silenciosamente errada. Num sensor de combate, "não vi ninguém" por defeito
de software é o pior modo de falha possível.

## Furtividade é um latch, não um comando

Descoberto durante os testes, e vale registrar porque não é óbvio: desligar
o emissor no laço principal **não funciona**. O ISR o religa na fronteira do
bloco seguinte, então um desligamento pontual suprime apenas 1 dos 2 blocos
de emissão. O teste mediu exatamente isso: 40 blocos ligados em vez de 80.

A correção é `stealth_hold`, um latch que o ISR consulta. Com ele o teste
mede 0 blocos de emissão sob ameaça.

Consequência em cascata que também precisou de guarda: com o emissor
apagado, a **calibração de crosstalk continuava acumulando** — e um ciclo sem
emissão entra na média como se fosse vazamento. O vetor calibrado saía
errado e o sintoma era um piso de proximidade que nunca some (o sensor
"vendo" alvo no vazio). Duas guardas resolveram: a calibração não acumula sob
`stealth_hold`, e a furtividade não atropela uma calibração em andamento.

## Como compilar

### Host (testes e bancada) — funciona já

```bash
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure

./build/sweep dist     # varre distância do alvo -> CSV
./build/sweep threat   # varre amplitude do emissor adversário
./build/sweep noise    # varre ruído do front-end
./build/sweep trace    # série temporal de um cenário de luta
```

### STM32G4 — precisa do projeto do CubeMX

Não há Makefile de ARM aqui de propósito: o CubeMX gera o dele, com os
caminhos de HAL, o linker script e o startup da variante exata. Brigar com
isso dá retrabalho a cada regeneração.

Para integrar:

1. Ajuste `firmware/platform/stm32/board_config.h` ao `.ioc`
   (itens marcados `[IOC]`).
2. Adicione ao projeto do CubeMX as fontes de `core/`, `app/app.c`,
   `app/main_stm32.c` e `platform/stm32/stm32_adapters.c`.
3. Adicione `firmware/` e `firmware/core` aos include paths.
4. No `main.c` gerado, depois dos `MX_*_Init()`: `sensor_app_init();`
   e no `while(1)`: `sensor_app_poll();`
5. Encaminhe os dois callbacks do ADC (o CubeMX não os gera) — o cabeçalho
   de `app/main_stm32.c` tem o código exato.

Os `_Static_assert` de `board_config.h` travam o build se o `.ioc` divergir
do plano de frequências, em vez de gerar firmware que mede errado em
silêncio.

## Interface com o micro principal

Cada sensor tem seu micro, então o principal precisa consultar N sensores.
I²C escravo é o caminho natural: um endereço por sensor, um barramento só
(`BOARD_REPORT_I2C_ADDR`). `sensor_report_t` em `app/app.h` é o layout fixo
e empacotado.

Duas regras para quem escrever o lado do micro principal:

- **`seq` incrementa por atualização e é escrito por último.** Compare `seq`
  antes e depois de ler o bloco; se mudou, releia. Detecta leitura rasgada.
- **Nunca ignore `flags`.** Um sensor saturado, cegado, sem calibração ou com
  overrun reporta proximidade baixa. Lido sem as flags, isso vira "não tem
  ninguém na minha frente".

O adapter de I²C escravo em si não está escrito — depende de como o
barramento do robô está montado (endereços, pull-ups, se há clock stretching).
É um arquivo pequeno em `platform/stm32/`, e o `sensor_report_t` já está
pronto para ser o buffer dele.
