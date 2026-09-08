/* stm32_adapters.h -- fachada dos adapters do G4.
 *
 * O app chama stm32_adapters_init() e recebe um ports_t pronto para
 * entregar ao core. Nada do core sabe que existe HAL.
 */

#ifndef STM32_ADAPTERS_H
#define STM32_ADAPTERS_H

#include <stdint.h>
#include <stdbool.h>
#include "../../port/ports.h"
#include "../../core/config/sensor_config.h"

/* Buffer do DMA: 2 blocos contiguos. O DMA roda CIRCULAR sobre os dois e
 * gera meia-transferencia no fim do 1o bloco e transferencia-completa no
 * fim do 2o. Assim o core sempre processa um bloco que o DMA nao esta
 * escrevendo -- double buffering sem trocar ponteiro na mao. */
#define STM32_DMA_BLOCKS  2u
extern uint16_t stm32_adc_buf[STM32_DMA_BLOCKS * CFG_BLOCK_N];

/* Monta os adapters e devolve os ports. Deve ser chamado DEPOIS do
 * MX_..._Init() gerado pelo CubeMX. */
void stm32_adapters_init(ports_t *out);

/* Arma timers, ADC e DMA e comeca a amostrar. Chamar depois de
 * ir_sensor_init(). */
void stm32_adapters_start(void);

/* Encaminha os callbacks do HAL para o core. Chame estas duas de dentro de
 * HAL_ADC_ConvHalfCpltCallback e HAL_ADC_ConvCpltCallback (ver
 * app/main_stm32.c). */
void stm32_on_adc_half(void);
void stm32_on_adc_full(void);

/* O core precisa ser alcancavel do ISO do DMA. Registrado aqui para nao
 * espalhar variavel global pelo projeto. */
struct ir_sensor;
void stm32_bind_sensor(struct ir_sensor *s);

#endif /* STM32_ADAPTERS_H */
