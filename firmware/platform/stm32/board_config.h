/* board_config.h -- TUDO o que sai do CubeMX mora aqui.
 *
 * >>> ESTE EH O UNICO ARQUIVO QUE PRECISA SER AJUSTADO AO .ioc DA PLACA. <<<
 *
 * O core (firmware/core/) e os limiares (sensor_config.h) nao dependem de
 * nada daqui. Se este arquivo estiver certo, o resto compila e roda.
 *
 * Cada item marcado [IOC] tem de casar com o projeto do CubeMX. Os valores
 * abaixo sao o que o plano de frequencias EXIGE (ver docs/DSP.md); se o .ioc
 * divergir, o build para nos _Static_assert do fim do arquivo em vez de
 * gerar firmware que mede errado em silencio.
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* ---------------------------------------------------------------------- */
/* [IOC] Familia                                                           */
/* ---------------------------------------------------------------------- */

/* O plano usa o OPAMP interno como PGA em modo high-speed -> STM32G4.
 * Troque o include se o .ioc apontar outra sub-familia (G431/G441/G474...). */
#include "stm32g4xx_hal.h"

/* ---------------------------------------------------------------------- */
/* [IOC] Arvore de clock                                                   */
/* ---------------------------------------------------------------------- */

/* TIMCLK precisa ser 144 MHz para que 90 kHz e 300 kSPS saiam EXATOS:
 *
 *   144e6 / 90e3  = 1600  (ARR da portadora)   -- inteiro
 *   144e6 / 300e3 =  480  (ARR do ADC)         -- inteiro
 *
 * Configuracao de PLL que da 144 MHz no G4:
 *   HSE 8 MHz : PLLM=/2 -> 4 MHz, PLLN=x72 -> 288 MHz (VCO ok), PLLR=/2 -> 144
 *   HSI 16MHz : PLLM=/4 -> 4 MHz, PLLN=x72 -> 288 MHz,            PLLR=/2 -> 144
 *
 * ATENCAO: 170 MHz (o maximo do G4) NAO serve -- 170e6/90e3 = 1888.9, nao
 * inteiro. Rodar a 170 MHz obriga a mexer no plano de frequencias inteiro.
 * Se precisar de 170 MHz por outro motivo, veja docs/DSP.md, "Outros clocks".
 */
#define BOARD_TIMCLK_HZ         144000000u

/* ---------------------------------------------------------------------- */
/* [IOC] Timer da portadora (PWM do LED emissor)                           */
/* ---------------------------------------------------------------------- */

/* TIM1_CH1 no default. Qualquer timer avancado/geral serve, desde que o
 * clock dele seja BOARD_TIMCLK_HZ. */
#define BOARD_EMIT_TIM              TIM1
#define BOARD_EMIT_TIM_CHANNEL      TIM_CHANNEL_1
#define BOARD_EMIT_TIM_HANDLE       htim1        /* declarado no main gerado */

/* ARR e CCR derivados -- NAO editar a mao, derivam do plano. */
#define BOARD_EMIT_ARR   ((BOARD_TIMCLK_HZ / CFG_CARRIER_HZ) - 1u)   /* 1599 */
#define BOARD_EMIT_CCR   ((BOARD_TIMCLK_HZ / CFG_CARRIER_HZ)          \
                          * CFG_EMITTER_DUTY_PCT / 100u)             /*  160 */

/* ---------------------------------------------------------------------- */
/* [IOC] Timer que dispara o ADC                                           */
/* ---------------------------------------------------------------------- */

/* TIM3 com TRGO no update, e o ADC com ExternalTrigConv = T3_TRGO.
 * O nome da constante do trigger MUDA entre familias -- confira no
 * stm32XXxx_hal_adc_ex.h da sua. */
#define BOARD_ADC_TIM               TIM3
#define BOARD_ADC_TIM_HANDLE        htim3
#define BOARD_ADC_TRIGGER           ADC_EXTERNALTRIG_T3_TRGO
#define BOARD_ADC_ARR    ((BOARD_TIMCLK_HZ / CFG_SAMPLE_RATE_HZ) - 1u) /* 479 */

/* ---------------------------------------------------------------------- */
/* [IOC] ADC + DMA                                                         */
/* ---------------------------------------------------------------------- */

#define BOARD_ADC                   ADC1
#define BOARD_ADC_HANDLE            hadc1
#define BOARD_ADC_DMA_HANDLE        hdma_adc1

/* Canal onde entra a saida do PGA. No G4 o OPAMP pode ir INTERNAMENTE ao
 * ADC (OPAMP2_VOUT -> ADC2_IN18, etc. -- ver tabela de conexoes internas do
 * RM). Preferir o caminho interno: nao gasta pino e nao pega ruido de placa. */
#define BOARD_ADC_CHANNEL           ADC_CHANNEL_3

/* Tempo de amostragem. Com ADCCLK a 60 MHz e Fs = 300 kSPS ha 200 ciclos de
 * ADCCLK por amostra, entao sobra folga. 24.5 ciclos = 408 ns de abertura:
 * atenua so 0.02 dB em 90 kHz (sinc), e ajuda um pouco no alias. */
#define BOARD_ADC_SAMPLETIME        ADC_SAMPLETIME_24CYCLES_5

/* NAO usar oversampling de hardware. Ele faz media de amostras
 * CONSECUTIVAS, o que no nosso caso mistura fases diferentes da portadora
 * (Fs/f0 = 10/3) e destroi exatamente a informacao de fase em que o lock-in
 * se sustenta. A media que queremos eh a coerente, feita no core. */

/* ---------------------------------------------------------------------- */
/* [IOC] PGA (OPAMP interno em modo high-speed)                            */
/* ---------------------------------------------------------------------- */

#define BOARD_PGA_ENABLE            1
#define BOARD_PGA_OPAMP             OPAMP2
#define BOARD_PGA_OPAMP_HANDLE      hopamp2

/* Modo high-speed eh OBRIGATORIO, nao opcional. Orcamento ganho-banda:
 *
 *   modo normal      GBW ~  1.6 MHz -> ganho 16 da  100 kHz de banda  X
 *   modo high-speed  GBW ~ 13   MHz -> ganho 16 da  810 kHz de banda  OK
 *                                      ganho 32 da  400 kHz de banda  OK
 *                                      ganho 64 da  200 kHz de banda  limite
 *
 * Com portadora em 90 kHz precisamos de banda >= ~3*f0 = 270 kHz para a
 * resposta ficar plana. Logo: high-speed sim, e ganho maximo 32.
 * Ver docs/HARDWARE.md. */
#define BOARD_PGA_HIGHSPEED         1

/* Mapa indice -> ganho. A ORDEM tem de ser crescente: o AGC do core sobe e
 * desce o indice assumindo isso. CFG_PGA_GAIN_COUNT em sensor_config.h tem
 * de casar com o tamanho desta lista. */
#define BOARD_PGA_GAIN_LIST { \
    OPAMP_PGA_GAIN_2_OR_MINUS_1,   \
    OPAMP_PGA_GAIN_8_OR_MINUS_7,   \
    OPAMP_PGA_GAIN_16_OR_MINUS_15, \
    OPAMP_PGA_GAIN_32_OR_MINUS_31  \
}

/* ---------------------------------------------------------------------- */
/* [IOC] UART de diagnostico (opcional)                                    */
/* ---------------------------------------------------------------------- */

#define BOARD_LOG_ENABLE            1
#define BOARD_LOG_UART_HANDLE       huart2

/* ---------------------------------------------------------------------- */
/* [IOC] Interface com o micro principal                                   */
/* ---------------------------------------------------------------------- */

/* Cada sensor tem seu proprio micro, entao o micro principal precisa
 * consultar N sensores. I2C escravo eh o caminho natural: um endereco por
 * sensor, um barramento so. Trocar o endereco por sensor. */
#define BOARD_REPORT_I2C_ADDR       0x42u

/* ---------------------------------------------------------------------- */
/* Sanidade -- trava o build se o .ioc divergir do plano                   */
/* ---------------------------------------------------------------------- */

#include "../../core/config/sensor_config.h"

_Static_assert(BOARD_TIMCLK_HZ % CFG_CARRIER_HZ == 0u,
    "TIMCLK nao gera a portadora de forma exata: o ARR daria fracionario e "
    "a portadora sairia deslocada do bin, matando a integracao coerente");

_Static_assert(BOARD_TIMCLK_HZ % CFG_SAMPLE_RATE_HZ == 0u,
    "TIMCLK nao gera Fs de forma exata: os bins deixam de ser inteiros");

_Static_assert(CFG_EMITTER_DUTY_PCT > 0u && CFG_EMITTER_DUTY_PCT < 100u,
    "duty do emissor tem de ficar entre 1 e 99 por cento");

_Static_assert((CFG_BLOCK_N * CFG_CARRIER_HZ) % CFG_SAMPLE_RATE_HZ == 0u,
    "N*f0/Fs nao eh inteiro: a fase da portadora nao se repete a cada bloco "
    "e a EWMA do lock-in passa a somar fasores girando");

#endif /* BOARD_CONFIG_H */
