/* main_stm32.c -- composition root do firmware do sensor.
 *
 * COMO ENCAIXAR NO PROJETO DO CUBEMX
 * ----------------------------------
 * O CubeMX gera o proprio main.c. Nao brigue com ele: mantenha o main.c
 * gerado e chame as tres funcoes daqui de dentro dele.
 *
 *   1. No main.c gerado, depois de todos os MX_*_Init():
 *
 *          sensor_app_init();
 *
 *   2. No while(1) do main.c gerado:
 *
 *          sensor_app_poll();
 *
 *   3. Encaminhe os callbacks do ADC (o CubeMX nao gera estes; adicione
 *      em main.c ou num arquivo seu):
 *
 *          void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *h)
 *          { (void)h; stm32_on_adc_half(); }
 *
 *          void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *h)
 *          { (void)h; stm32_on_adc_full(); }
 *
 * Por que nao um main() proprio: o main.c gerado eh reescrito a cada
 * regeneracao do .ioc. Deixando o codigo do sensor fora dele, regenerar o
 * projeto nunca apaga trabalho.
 */

#include "app.h"
#include "../platform/stm32/stm32_adapters.h"
#include "../core/sensor/ir_sensor.h"

static ir_sensor_t g_sensor;
static app_t       g_app;

void sensor_app_init(void)
{
    ports_t ports;
    stm32_adapters_init(&ports);

    ir_sensor_init(&g_sensor, &ports);
    stm32_bind_sensor(&g_sensor);

    app_init(&g_app, &g_sensor, &ports);

    stm32_adapters_start();

    /* Calibracao de crosstalk na largada.
     *
     * IMPORTANTE: o sensor tem de estar apontado para o VAZIO neste
     * momento (nada a menos de ~1 m). Na pratica: ligue o robo fora do
     * dohyo, ou dispare a calibracao por comando do micro principal antes
     * da luta. Calibrar com alvo na frente grava o alvo como se fosse
     * vazamento interno e o sensor fica cego para ele. */
    ir_sensor_calibrate(&g_sensor);
}

void sensor_app_poll(void)
{
    app_poll(&g_app);
}
