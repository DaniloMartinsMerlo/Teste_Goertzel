/* sensor_config.h -- plano de frequencias e parametros de decisao do sensor.
 *
 * Este eh o unico arquivo que se mexe para "afinar" o sensor. Nada aqui
 * depende de STM32/HAL: sao os numeros do DSP e os limiares de decisao.
 *
 * Pinos, instancias de timer/ADC e clocks NAO ficam aqui -- ficam em
 * firmware/platform/stm32/board_config.h (o que sai do CubeMX).
 *
 * A derivacao completa destes numeros esta em docs/DSP.md. Resumo:
 *
 *   TIMCLK = 144 MHz  (PLL: HSE 8 MHz /2 *72 /2, ou HSI16 /4 *72 /2)
 *   Fs     = 300 kSPS  -> ARR do timer do ADC     = 144e6/300e3 = 480  (exato)
 *   f0     =  90 kHz   -> ARR do timer da portadora = 144e6/90e3 = 1600 (exato)
 *   N      = 300 amostras/bloco -> bloco = 1.000 ms, df = Fs/N = 1000 Hz
 *
 * Com df = 1 kHz exato, TODA frequencia de interesse cai em bin inteiro
 * (30/33/36/38/40/56/90 kHz) => perda de scalloping = 0 dB e integracao
 * coerente exata. N*f0/Fs = 90 ciclos inteiros por bloco, o que garante
 * que a fase da portadora se repete a cada bloco -- condicao necessaria
 * para o lock-in acumular blocos de forma coerente.
 */

#ifndef SENSOR_CONFIG_H
#define SENSOR_CONFIG_H

/* ---------------------------------------------------------------------- */
/* Plano de amostragem                                                     */
/* ---------------------------------------------------------------------- */

#define CFG_SAMPLE_RATE_HZ      300000u
#define CFG_BLOCK_N             300u        /* 1.000 ms por bloco          */
#define CFG_BIN_HZ              (CFG_SAMPLE_RATE_HZ / CFG_BLOCK_N)  /* 1000 */

/* ---------------------------------------------------------------------- */
/* ETAPA C -- portadora propria (emissor + lock-in)                        */
/* ---------------------------------------------------------------------- */

/* Bin da nossa portadora. 90 => 90 kHz.
 *
 * 90 kHz fica MUITO acima de toda a grade padrao de receptores IR
 * (30/33/36/36.7/38/40/56 kHz). Um TSOP/TSSP nao tem ganho nenhum ali e o
 * AGC dele trata portadora continua como ruido ambiente. Ou seja: o
 * adversario nao nos ve, e ainda assim nos vemos ele (ETAPA B) e vemos o
 * nosso proprio reflexo (ETAPA C).
 *
 * FDMA entre os nossos proprios sensores: cada sensor recebe um bin
 * diferente desta lista, todos exatos a partir de 144 MHz e todos em bin
 * inteiro (ver docs/DSP.md, secao "FDMA"):
 *
 *     bin  72 ->  72 kHz  (ARR 2000)
 *     bin  80 ->  80 kHz  (ARR 1800)
 *     bin  90 ->  90 kHz  (ARR 1600)   <-- default
 *     bin  96 ->  96 kHz  (ARR 1500)
 *     bin 100 -> 100 kHz  (ARR 1440)
 *     bin 120 -> 120 kHz  (ARR 1200)
 */
#ifndef CFG_CARRIER_BIN
#define CFG_CARRIER_BIN         90u
#endif
#define CFG_CARRIER_HZ          (CFG_CARRIER_BIN * CFG_BIN_HZ)

/* Duty do PWM do emissor, em porcento. O plano pede ~10%: mantem a corrente
 * media do LED baixa (termico) permitindo pico alto (alcance).
 *
 * ATENCAO: duty 10% gera 3a harmonica a apenas -1.2 dBc. Em Fs = 300 kSPS
 * ela dobra para 30 kHz (bin 30), NAO para cima da portadora -- por isso
 * Fs = 300k e nao 360k (= 4*f0). Ver docs/DSP.md, "Aliasing de harmonicas".
 */
#define CFG_EMITTER_DUTY_PCT    10u

/* ---------------------------------------------------------------------- */
/* ETAPA B -- varredura passiva de emissores adversarios                   */
/* ---------------------------------------------------------------------- */

/* Bins vigiados. O plano pede 38 e 56 kHz; os demais entram porque custam
 * ~0.1% de CPU cada e cobrem o resto da grade comercial de TSOP.
 *
 * Bin 30 (30 kHz) fica FORA da lista por um motivo concreto: eh onde a 3a
 * harmonica do nosso proprio emissor dobra. Se um dia for preciso vigiar
 * 30 kHz, so eh confiavel em bloco com emissor desligado (que eh o caso na
 * ETAPA B, mas o guard fica documentado em threat_scan.c).
 */
#define CFG_THREAT_BINS         { 38u, 56u, 36u, 40u, 33u }
#define CFG_THREAT_COUNT        5u

/* Bin de referencia de ruido: nada esperado em 140 kHz (acima da portadora,
 * abaixo de Nyquist = 150 kHz). Usado para limiar adaptativo / estimar SNR
 * em vez de comparar contra um numero magico. */
#define CFG_NOISE_REF_BIN       140u

/* Roda a varredura de ameaca 1 em cada N blocos de escuta. 1 = todo bloco. */
#define CFG_THREAT_DIVIDER      1u

/* ---------------------------------------------------------------------- */
/* Ciclo de chopping (deriva direto do fluxo do plano)                     */
/* ---------------------------------------------------------------------- */

/* O fluxo do plano ja alterna naturalmente emissor OFF (etapas A e B) e
 * emissor ON (etapa C). Aproveitamos isso como chopper: o bloco OFF mede o
 * fundo na NOSSA propria frequencia e o subtraimos vetorialmente do bloco
 * ON. Isso derruba interferencia estatica em 90 kHz (outro robo na mesma
 * frequencia, ou vazamento de um sensor vizinho nosso).
 *
 * Blocos descartados apos cada transicao do emissor, para o front-end
 * analogico (TIA + PGA) assentar. 1 bloco = 1 ms, folga enorme para um
 * front-end de ~900 kHz -- mas cobre tambem o settling do LED e do driver.
 */
#define CFG_SETTLE_BLOCKS       1u

/* Ciclo completo: OFF_LISTEN, ON_SETTLE, ON_MEASURE, OFF_SETTLE = 4 ms
 * => 250 decisoes/s. Um robo a 1 m/s anda 4 mm entre decisoes. */

/* ---------------------------------------------------------------------- */
/* ETAPA A -- cegamento / inimigo colado                                   */
/* ---------------------------------------------------------------------- */

/* Media CC do bloco acima disto = alguem jogando luz IR forte em cima de
 * nos (ou inimigo colado). Em contagens de ADC de 12 bits (0..4095).
 * O front-end deve estar centrado em ~VREF/2 = 2048, entao o desvio eh o
 * que importa -- ver CFG_DC_NOMINAL. */
#define CFG_DC_NOMINAL          2048u
#define CFG_DC_BLIND_DELTA      1200u   /* |media - nominal| > isto = cego  */

/* Amostras a >= este valor (ou <= 4095 - isto) contam como saturadas.      */
#define CFG_SAT_MARGIN          32u
/* Fracao de amostras saturadas no bloco (em 1/256) que condena o bloco.    */
#define CFG_SAT_FRACTION_Q8     8u      /* ~3%                              */

/* ---------------------------------------------------------------------- */
/* Furtividade                                                             */
/* ---------------------------------------------------------------------- */

/* 1 = ao detectar emissor adversario (ETAPA B), NAO acende o nosso LED.
 * Eh o "Furtividade Mantida!" do plano, e eh o default.
 *
 * Troca consciente: enquanto a ameaca durar, a ETAPA C nao roda e o sensor
 * perde a medida de distancia -- reporta IR_VERDICT_ENEMY_GOERTZEL sem
 * proximidade. Ponha 0 se preferir alcance a furtividade. */
#define CFG_STEALTH_ON_THREAT   1

/* ---------------------------------------------------------------------- */
/* Filtro pos-deteccao do lock-in (EWMA complexa sobre I/Q)                */
/* ---------------------------------------------------------------------- */

/* tau = 2^shift * periodo_do_ciclo. Ciclo = 4 ms, entao:
 *   shift 0 -> tau =  4 ms  (ENBW ~62 Hz)
 *   shift 1 -> tau =  8 ms  (ENBW ~31 Hz)
 *   shift 2 -> tau = 16 ms  (ENBW ~16 Hz)   <-- default
 *   shift 3 -> tau = 32 ms  (ENBW ~ 8 Hz)
 * Mais shift = mais alcance e menos ruido, mas mais atraso. Para sumo,
 * 16 ms ja eh conservador; baixe para 1 se precisar de reflexo mais rapido. */
#define CFG_LOCKIN_LPF_SHIFT    2u

/* ---------------------------------------------------------------------- */
/* Decisao de presenca (histerese + votacao temporal)                      */
/* ---------------------------------------------------------------------- */

/* Limiares em SNR (magnitude do sinal / piso de ruido), em Q8.
 * 6.0 em Q8 = 1536. Histerese evita chattering na borda do alcance. */
#define CFG_PRESENCE_SNR_ON_Q8   1536u   /* 6.0x  (~15.6 dB) */
#define CFG_PRESENCE_SNR_OFF_Q8   768u   /* 3.0x  (~9.5 dB)  */

/* Votacao M-de-N sobre as decisoes instantaneas. */
#define CFG_PRESENCE_VOTE_N      5u
#define CFG_PRESENCE_VOTE_M      3u

/* Idem para a ETAPA B (deteccao do emissor adversario). Potencia, nao
 * amplitude, entao o limiar eh em potencia relativa ao piso, Q8. */
#define CFG_THREAT_SNR_ON_Q8     2048u   /* 8.0x em potencia (~9 dB)  */
#define CFG_THREAT_SNR_OFF_Q8    1024u   /* 4.0x                      */
/* Peak-hold para pegar emissores adversarios que transmitem em rajada
 * (protocolo TSOP tipico): quantos ciclos a deteccao fica retida. */
#define CFG_THREAT_HOLD_CYCLES   8u      /* 8 * 4 ms = 32 ms */

/* ---------------------------------------------------------------------- */
/* PGA (OPAMP interno do G4 em modo high-speed) -- AGC                     */
/* ---------------------------------------------------------------------- */

/* Ganhos disponiveis, em ordem crescente. Indices, nao valores: o mapa para
 * OPAMP_PGA_GAIN_x fica no adapter. Ver docs/HARDWARE.md para o orcamento
 * de ganho-banda: em high-speed mode (GBW ~13 MHz) o ganho 32 ainda deixa
 * ~400 kHz de banda, suficiente para 90 kHz. Ganho 64 NAO fecha. */
#define CFG_PGA_GAIN_COUNT      4u      /* {2, 8, 16, 32} */
#define CFG_PGA_GAIN_DEFAULT    2u      /* indice -> ganho 16 */

/* AGC: sobe o ganho se o SNR ficar abaixo disto, desce se saturar. */
#define CFG_AGC_ENABLE          1
#define CFG_AGC_SNR_LOW_Q8      1024u   /* 4.0x  -> tenta subir ganho */
#define CFG_AGC_SETTLE_CYCLES   4u      /* ciclos ignorados apos trocar    */

/* ---------------------------------------------------------------------- */
/* Sanidade                                                                */
/* ---------------------------------------------------------------------- */

/* Fs/N deve dar inteiro, senao os bins nao sao inteiros e a integracao
 * coerente entre blocos deixa de valer. */
#if (CFG_SAMPLE_RATE_HZ % CFG_BLOCK_N) != 0
#error "CFG_SAMPLE_RATE_HZ deve ser multiplo de CFG_BLOCK_N (bin inteiro)"
#endif

#if CFG_CARRIER_BIN >= (CFG_BLOCK_N / 2)
#error "CFG_CARRIER_BIN acima de Nyquist"
#endif

#if CFG_NOISE_REF_BIN >= (CFG_BLOCK_N / 2)
#error "CFG_NOISE_REF_BIN acima de Nyquist"
#endif

#if CFG_PRESENCE_VOTE_M > CFG_PRESENCE_VOTE_N
#error "CFG_PRESENCE_VOTE_M nao pode ser maior que CFG_PRESENCE_VOTE_N"
#endif

#endif /* SENSOR_CONFIG_H */
