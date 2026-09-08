/* threat_scan.h -- ETAPA B do plano: escuta PASSIVA de emissores adversarios.
 *
 * Roda um Goertzel por bin vigiado (38 e 56 kHz, mais o resto da grade
 * comercial) sobre o mesmo bloco de ADC, com o NOSSO emissor DESLIGADO.
 *
 * Duas propriedades que valem a pena entender:
 *
 * 1. FURTIVIDADE. Detectar o adversario pela emissao DELE nao emite nada.
 *    Enquanto a ETAPA B acha alguem, o fluxo do plano nunca chega na
 *    ETAPA C -- ou seja, nunca ligamos o LED. Achamos ele sem ele achar
 *    nos.
 *
 * 2. ALCANCE. Emissao propria eh caminho de IDA E VOLTA (~1/d^4 para alvo
 *    difuso); escutar o emissor dele eh caminho de IDA SO (~1/d^2). Logo a
 *    ETAPA B ve mais longe que a ETAPA C. Isso nao eh bonus: eh o motivo
 *    de a ETAPA B vir antes na cascata.
 *
 * Deteccao INCOERENTE: o emissor do adversario nao tem relacao de fase
 * nenhuma com o nosso clock. Entao a media eh de POTENCIA (|X|^2) entre
 * blocos, nunca de fasor -- somar I/Q de uma fonte de fase aleatoria da
 * zero. Por isso threat_scan usa gz_power() e nao gz_phasor().
 *
 * Peak-hold: receptor IR comercial trabalha em rajada (protocolo tipo
 * TSOP: ~10 ciclos ligados, gap, repete). Sem hold, a deteccao pisca junto
 * com a rajada. O hold retem por CFG_THREAT_HOLD_CYCLES.
 */

#ifndef THREAT_SCAN_H
#define THREAT_SCAN_H

#include <stdint.h>
#include <stdbool.h>
#include "../dsp/goertzel.h"
#include "../config/sensor_config.h"
#include "hysteresis.h"

typedef struct {
    uint16_t bin;          /* bin vigiado                              */
    uint16_t freq_khz;     /* frequencia, para o relatorio             */
    uint32_t magnitude;    /* |X| do ultimo bloco medido               */
    uint16_t snr_q8;       /* potencia / piso de ruido, Q8             */
    bool     detected;     /* apos histerese + peak-hold               */
    uint8_t  hold;         /* ciclos restantes de peak-hold            */
} threat_band_t;

typedef struct {
    gz_t          gz[CFG_THREAT_COUNT];
    gz_t          gz_noise;                    /* bin de referencia    */
    threat_band_t band[CFG_THREAT_COUNT];
    hyst_t        hyst[CFG_THREAT_COUNT];

    uint32_t      noise_floor;   /* |X| do bin de referencia, filtrado */
    fx_ewma_t     noise_lpf;

    uint8_t       count;
    uint8_t       divider;
    uint8_t       div_count;
    bool          any_detected;
    uint8_t       strongest;     /* indice da banda mais forte         */
} threat_scan_t;

void threat_scan_init(threat_scan_t *t);

/* Processa um bloco de escuta (emissor DESLIGADO). x ja sem CC.
 * Devolve true se alguma banda esta em deteccao. */
bool threat_scan_process(threat_scan_t *t, const int16_t *x, uint16_t n);

/* Piso de ruido corrente (|X| do bin de referencia), util para limiar
 * adaptativo em outras camadas. */
uint32_t threat_scan_noise_floor(const threat_scan_t *t);

#endif /* THREAT_SCAN_H */
