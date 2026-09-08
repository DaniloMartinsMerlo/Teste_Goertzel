/* app.h -- camada de aplicacao: liga o core aos ports e monta o relatorio
 * que vai para o micro principal.
 *
 * Independente de plataforma: roda no G4 e no PC. Quem cria os ports eh o
 * main_* de cada plataforma.
 */

#ifndef APP_H
#define APP_H

#include <stdint.h>
#include "../core/sensor/ir_sensor.h"
#include "../port/ports.h"

/* Relatorio lido pelo micro principal (via I2C/UART). Layout FIXO e
 * empacotado: o micro principal le isto como bloco de bytes.
 *
 * Regras deste struct:
 *   - little-endian (nativo em Cortex-M);
 *   - `seq` incrementa a cada atualizacao -- o leitor descarta leitura
 *     rasgada comparando seq antes e depois;
 *   - `flags` NUNCA deve ser ignorado pelo micro principal. Um sensor
 *     saturado, cegado ou sem calibracao reporta proximidade baixa, que
 *     lido sem as flags vira "nao tem ninguem na minha frente" -- o pior
 *     modo de falha possivel numa luta.
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;            /* 0x5A -- sanidade do barramento          */
    uint8_t  version;          /* 1                                       */
    uint16_t seq;

    uint8_t  verdict;          /* ir_verdict_t                            */
    uint8_t  presence;         /* 0/1                                     */
    uint16_t flags;            /* IR_FLAG_*                               */

    uint32_t proximity;        /* monotonico com a proximidade, nao mm    */
    uint16_t snr_q8;
    int16_t  phase_q15;        /* fase do lock-in / 2 (cabe em int16)     */

    uint16_t noise_floor;
    uint16_t dc_mean;
    uint16_t jam_magnitude;

    uint8_t  threat_mask;      /* bit i = banda i detectada               */
    uint8_t  threat_freq_khz;  /* frequencia da ameaca mais forte         */
    uint8_t  pga_gain_index;
    uint8_t  reserved;
} sensor_report_t;

typedef struct {
    ir_sensor_t *sensor;
    ports_t      ports;
    sensor_report_t report;
    uint32_t     last_log_cycle;
} app_t;

void app_init(app_t *a, ir_sensor_t *sensor, const ports_t *ports);

/* Chamar do laco principal, sem delay. Drena os blocos pendentes, roda o
 * DSP e atualiza o relatorio. */
void app_poll(app_t *a);

const sensor_report_t *app_report(const app_t *a);

/* Pontos de entrada usados pelo main_stm32.c. */
void sensor_app_init(void);
void sensor_app_poll(void);

#endif /* APP_H */
