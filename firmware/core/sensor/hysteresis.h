/* hysteresis.h -- decisor booleano com histerese + votacao M-de-N.
 *
 * Duas travas contra falso positivo/negativo, que num robo de sumo custam
 * a luta:
 *
 *   histerese  -> dois limiares (liga alto, desliga baixo). Sem isto, um
 *                 alvo na borda do alcance faz o sensor piscar.
 *   votacao    -> so muda de estado se M das ultimas N amostras
 *                 concordarem. Mata glitch de bloco unico (LED do
 *                 adversario piscando, faisca de motor).
 */

#ifndef HYSTERESIS_H
#define HYSTERESIS_H

#include <stdint.h>
#include <stdbool.h>

#define HYST_MAX_WINDOW  16u

typedef struct {
    uint16_t thr_on;    /* >= liga  */
    uint16_t thr_off;   /* <  desliga (thr_off <= thr_on) */
    uint8_t  vote_n;
    uint8_t  vote_m;

    uint16_t window;    /* bitmap das ultimas vote_n decisoes brutas */
    uint8_t  filled;
    bool     state;
} hyst_t;

void hyst_init(hyst_t *h, uint16_t thr_on, uint16_t thr_off,
               uint8_t vote_n, uint8_t vote_m);

void hyst_reset(hyst_t *h);

/* Alimenta uma medida e devolve o estado filtrado. */
bool hyst_push(hyst_t *h, uint16_t value);

bool hyst_state(const hyst_t *h);

#endif /* HYSTERESIS_H */
