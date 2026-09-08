/* stm32_adapters.c -- a UNICA parte do firmware que fala HAL/CubeMX.
 *
 * Responsabilidades:
 *   - PWM da portadora (emissor)                -> port_emitter
 *   - ganho do OPAMP interno em modo PGA        -> port_pga
 *   - base de tempo                             -> port_time
 *   - UART de diagnostico                       -> port_log
 *   - ADC + DMA circular de bloco duplo         -> alimenta o core
 *
 * Se um dia o projeto mudar de familia (ou de micro), este arquivo e o
 * board_config.h sao o unico trabalho. O core nao muda uma linha.
 */

#include "stm32_adapters.h"
#include "board_config.h"
#include "../../core/sensor/ir_sensor.h"

#include <stdio.h>
#include <string.h>

/* Handles gerados pelo CubeMX (declarados em main.c / stm32g4xx_hal_msp.c). */
extern ADC_HandleTypeDef   BOARD_ADC_HANDLE;
extern TIM_HandleTypeDef   BOARD_EMIT_TIM_HANDLE;
extern TIM_HandleTypeDef   BOARD_ADC_TIM_HANDLE;
#if BOARD_PGA_ENABLE
extern OPAMP_HandleTypeDef BOARD_PGA_OPAMP_HANDLE;
#endif
#if BOARD_LOG_ENABLE
extern UART_HandleTypeDef  BOARD_LOG_UART_HANDLE;
#endif

/* Alinhado: em algumas familias o DMA para periferico de 16 bits exige
 * alinhamento de meia-palavra, e o cache-less do G4 nao perdoa desalinho. */
uint16_t stm32_adc_buf[STM32_DMA_BLOCKS * CFG_BLOCK_N] __attribute__((aligned(4)));

static ir_sensor_t *s_sensor;

void stm32_bind_sensor(struct ir_sensor *s)
{
    s_sensor = (ir_sensor_t *)s;
}

/* ---------------------------------------------------------------------- */
/* Emissor                                                                 */
/* ---------------------------------------------------------------------- */

/* Liga/desliga a portadora. Chamado de dentro do ISR do DMA, na fronteira
 * do bloco -- por isso mexe direto no registrador em vez de passar por
 * HAL_TIM_PWM_Start/Stop, que fazem varias leituras-modificacoes e
 * gastariam microssegundos dentro do ISR.
 *
 * Zerar/restaurar o CCR (em vez de parar o timer) mantem o timer contando,
 * o que preserva a relacao de fase com o timer do ADC. Parar e religar o
 * timer perderia essa fase e o lock-in mediria uma fase diferente a cada
 * ciclo de chopping -- a EWMA sobre I/Q daria zero. */
static void emitter_set_impl(void *ctx, bool on)
{
    (void)ctx;
    __HAL_TIM_SET_COMPARE(&BOARD_EMIT_TIM_HANDLE, BOARD_EMIT_TIM_CHANNEL,
                          on ? BOARD_EMIT_CCR : 0u);
}

/* ---------------------------------------------------------------------- */
/* PGA                                                                     */
/* ---------------------------------------------------------------------- */

#if BOARD_PGA_ENABLE
static const uint32_t k_pga_gains[] = BOARD_PGA_GAIN_LIST;

_Static_assert(sizeof(k_pga_gains) / sizeof(k_pga_gains[0])
                   == CFG_PGA_GAIN_COUNT,
    "BOARD_PGA_GAIN_LIST e CFG_PGA_GAIN_COUNT divergem: o AGC do core "
    "indexaria fora da lista");

static void pga_set_gain_impl(void *ctx, uint8_t idx)
{
    (void)ctx;
    if (idx >= CFG_PGA_GAIN_COUNT) {
        idx = CFG_PGA_GAIN_COUNT - 1u;
    }

    /* O OPAMP tem de sair de operacao para o ganho ser reprogramado. Isso
     * gera um transiente de alguns microssegundos -- e por isso o core
     * mantem CFG_AGC_SETTLE_CYCLES ciclos de quarentena depois de trocar. */
    HAL_OPAMP_Stop(&BOARD_PGA_OPAMP_HANDLE);
    BOARD_PGA_OPAMP_HANDLE.Init.PgaGain = k_pga_gains[idx];
    (void)HAL_OPAMP_Init(&BOARD_PGA_OPAMP_HANDLE);
    HAL_OPAMP_Start(&BOARD_PGA_OPAMP_HANDLE);
}
#endif

/* ---------------------------------------------------------------------- */
/* Tempo e log                                                             */
/* ---------------------------------------------------------------------- */

static uint32_t now_us_impl(void *ctx)
{
    (void)ctx;
    /* DWT->CYCCNT da resolucao de ciclo sem gastar um timer. Habilitado em
     * stm32_adapters_init(). */
    return DWT->CYCCNT / (BOARD_TIMCLK_HZ / 1000000u);
}

#if BOARD_LOG_ENABLE
static void log_write_impl(void *ctx, const char *line)
{
    (void)ctx;
    size_t n = strlen(line);
    /* Bloqueante de proposito: log eh ferramenta de bancada, e usar DMA
     * aqui competiria com o DMA do ADC por prioridade de barramento. Nunca
     * chamar do ISR. */
    (void)HAL_UART_Transmit(&BOARD_LOG_UART_HANDLE, (uint8_t *)line,
                            (uint16_t)n, 100u);
}
#endif

/* ---------------------------------------------------------------------- */
/* ADC + DMA                                                               */
/* ---------------------------------------------------------------------- */

void stm32_on_adc_half(void)
{
    /* Primeira metade cheia: o DMA agora escreve na segunda. */
    if (s_sensor != NULL) {
        ir_sensor_on_block_isr(s_sensor, &stm32_adc_buf[0], CFG_BLOCK_N);
    }
}

void stm32_on_adc_full(void)
{
    if (s_sensor != NULL) {
        ir_sensor_on_block_isr(s_sensor, &stm32_adc_buf[CFG_BLOCK_N],
                               CFG_BLOCK_N);
    }
}

/* ---------------------------------------------------------------------- */

void stm32_adapters_init(ports_t *out)
{
    memset(out, 0, sizeof(*out));

    /* Habilita o contador de ciclos para now_us_impl(). */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    out->emitter.set  = emitter_set_impl;
    out->time.now_us  = now_us_impl;

#if BOARD_PGA_ENABLE
    out->pga.set_gain = pga_set_gain_impl;
#endif
#if BOARD_LOG_ENABLE
    out->log.write    = log_write_impl;
#endif
}

void stm32_adapters_start(void)
{
    /* --- portadora ---------------------------------------------------- */
    __HAL_TIM_SET_AUTORELOAD(&BOARD_EMIT_TIM_HANDLE, BOARD_EMIT_ARR);
    __HAL_TIM_SET_COMPARE(&BOARD_EMIT_TIM_HANDLE, BOARD_EMIT_TIM_CHANNEL, 0u);
    __HAL_TIM_SET_COUNTER(&BOARD_EMIT_TIM_HANDLE, 0u);

    /* --- timer do ADC ------------------------------------------------- */
    __HAL_TIM_SET_AUTORELOAD(&BOARD_ADC_TIM_HANDLE, BOARD_ADC_ARR);
    __HAL_TIM_SET_COUNTER(&BOARD_ADC_TIM_HANDLE, 0u);

#if BOARD_PGA_ENABLE
    HAL_OPAMP_Start(&BOARD_PGA_OPAMP_HANDLE);
#endif

    /* --- ADC + DMA circular ------------------------------------------- */
    (void)HAL_ADCEx_Calibration_Start(&BOARD_ADC_HANDLE, ADC_SINGLE_ENDED);
    (void)HAL_ADC_Start_DMA(&BOARD_ADC_HANDLE, (uint32_t *)stm32_adc_buf,
                            STM32_DMA_BLOCKS * CFG_BLOCK_N);

    /* --- larga os dois timers "juntos" -------------------------------- */
    /* Os dois contadores foram zerados acima e saem daqui com poucos ciclos
     * de diferenca. Isso NAO eh problema: como os dois vem do mesmo clock
     * com ARR inteiros (1600 e 480), a defasagem inicial eh CONSTANTE e
     * nunca deriva. Uma fase constante eh exatamente o que a calibracao de
     * crosstalk absorve.
     *
     * Se quiser partida deterministica: configure o timer da portadora como
     * mestre (TRGO no enable) e o do ADC como escravo em modo gated. Nao eh
     * necessario para a medida ser correta. */
    (void)HAL_TIM_PWM_Start(&BOARD_EMIT_TIM_HANDLE, BOARD_EMIT_TIM_CHANNEL);
    (void)HAL_TIM_Base_Start(&BOARD_ADC_TIM_HANDLE);
}
