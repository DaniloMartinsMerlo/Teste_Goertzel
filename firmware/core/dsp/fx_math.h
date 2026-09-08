/* fx_math.h -- aritmetica de ponto fixo usada pelo DSP do sensor.
 *
 * Sem float, sem libm, sem divisao de 64 bits nos caminhos quentes.
 * Roda igual em Cortex-M0 e em M4F, e roda no PC nos testes.
 */

#ifndef FX_MATH_H
#define FX_MATH_H

#include <stdint.h>

/* Fasor em ponto fixo. Escala arbitraria (contagens de ADC * N/2). */
typedef struct {
    int32_t re;
    int32_t im;
} fx_cplx_t;

/* Raiz quadrada inteira de 64 bits, truncada. Newton com semente por
 * bit-length: converge em <= 6 iteracoes para qualquer entrada. */
uint32_t fx_isqrt64(uint64_t v);

/* |z| = sqrt(re^2 + im^2). Satura em UINT32_MAX. */
uint32_t fx_cplx_abs(fx_cplx_t z);

/* |z|^2, exato em 64 bits. */
uint64_t fx_cplx_abs2(fx_cplx_t z);

/* atan2(im, re) em Q15 radianos, faixa (-pi, +pi] -> (-102944, +102944].
 * Aproximacao polinomial de 3 termos, erro maximo ~0.0015 rad (0.086 deg).
 * Usado uma vez por bloco: custo irrelevante, e nao puxa libm. */
int32_t fx_atan2_q15(int32_t im, int32_t re);

/* Divisao com resultado em Q8, saturada em UINT16_MAX. Usada para SNR.
 * den == 0 devolve UINT16_MAX (satura em vez de estourar). */
uint16_t fx_div_q8(uint32_t num, uint32_t den);

/* EWMA de 1 polo com acumulador estendido, para nao ter zona morta:
 *   acc += x - (acc >> shift);   out = acc >> shift
 * Constante de tempo tau = 2^shift periodos de amostragem.
 * O acumulador guarda x << shift, entao o chamador deve dimensionar
 * shift de modo que |x| << shift caiba em int32. */
typedef struct {
    int32_t acc;
    uint8_t shift;
    uint8_t primed;
} fx_ewma_t;

void    fx_ewma_init(fx_ewma_t *f, uint8_t shift);
int32_t fx_ewma_push(fx_ewma_t *f, int32_t x);
int32_t fx_ewma_get(const fx_ewma_t *f);

#endif /* FX_MATH_H */
