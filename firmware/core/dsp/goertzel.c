#include "goertzel.h"
#include "fx_tables.h"
#include "../config/sensor_config.h"

/* As tabelas sao geradas para um N/Fs especifico. Se sensor_config.h e
 * fx_tables.h divergirem, os bins deixam de ser inteiros silenciosamente
 * -- e o lock-in perde coerencia entre blocos sem dar erro nenhum. Trava
 * o build em vez de deixar passar. */
_Static_assert(FX_TABLE_N == CFG_BLOCK_N,
               "fx_tables.h desatualizado: rode tools/gen_dsp_tables.py");
_Static_assert(FX_TABLE_FS == CFG_SAMPLE_RATE_HZ,
               "fx_tables.h desatualizado: rode tools/gen_dsp_tables.py");

void gz_init(gz_t *g, uint16_t bin)
{
    if (bin >= FX_TABLE_BINS) {
        bin = FX_TABLE_BINS - 1u;
    }

    g->bin       = bin;
    g->coeff = fx_goertzel_coeff[bin];
    g->cos_q15   = fx_cos_q15[bin];
    g->sin_q15   = fx_sin_q15[bin];
    gz_reset(g);
}

void gz_reset(gz_t *g)
{
    g->s1   = 0;
    g->s2   = 0;
    g->peak = 0u;
}

void gz_push(gz_t *g, int32_t x)
{
    int32_t s0 = x + (int32_t)(((int64_t)g->coeff * g->s1) >> FX_COEFF_Q)
                   - g->s2;
    g->s2 = g->s1;
    g->s1 = s0;

    uint32_t a = (uint32_t)((s0 < 0) ? -s0 : s0);
    if (a > g->peak) {
        g->peak = a;
    }
}

void gz_push_block(gz_t *g, const int16_t *x, uint16_t n)
{
    /* Copias locais: o compilador mantem em registrador e o laco fica
     * sem load/store de estado. */
    const int32_t coeff = g->coeff;
    int32_t s1 = g->s1;
    int32_t s2 = g->s2;
    uint32_t peak = g->peak;

    for (uint16_t i = 0u; i < n; i++) {
        int32_t s0 = (int32_t)x[i]
                   + (int32_t)(((int64_t)coeff * s1) >> FX_COEFF_Q)
                   - s2;
        s2 = s1;
        s1 = s0;

        uint32_t a = (uint32_t)((s0 < 0) ? -s0 : s0);
        if (a > peak) {
            peak = a;
        }
    }

    g->s1   = s1;
    g->s2   = s2;
    g->peak = peak;
}

fx_cplx_t gz_phasor(const gz_t *g)
{
    /* e^(-jw) = cos(w) - j*sin(w), logo
     *   Y = s1 - e^(-jw)*s2 = (s1 - cos(w)*s2) + j*(+sin(w)*s2)
     * O sinal do termo imaginario eh POSITIVO: sai da distributiva do
     * -j*sin(w) contra o -s2. Errar isto conjuga o fasor, o que passa
     * despercebido em magnitude e destroi a fase (e portanto a subtracao
     * vetorial do crosstalk). test_dsp compara contra uma DFT em double
     * exatamente para travar este sinal. */
    fx_cplx_t z;
    z.re = g->s1 - (int32_t)(((int64_t)g->cos_q15 * g->s2) >> FX_TRIG_Q);
    z.im =         (int32_t)(((int64_t)g->sin_q15 * g->s2) >> FX_TRIG_Q);
    return z;
}

uint64_t gz_power(const gz_t *g)
{
    int64_t s1 = g->s1;
    int64_t s2 = g->s2;

    /* |Y|^2 = s1^2 + s2^2 - 2cos(w)*s1*s2. O termo cruzado eh subtraido,
     * entao o resultado eh sempre >= 0 na matematica exata; com o coeff
     * quantizado em Q11 ele pode ficar levemente negativo quando s1 e s2
     * sao quase colineares (sinal fraquissimo). Ceifa em 0.
     *
     * Multiplica antes de deslocar: s1*s2 chega a 2^37 e coeff a 2^12,
     * logo o produto vai a 2^49 -- sobra folga no int64 e nao se perde
     * precisao para deslocar depois. */
    int64_t cross = ((int64_t)g->coeff * s1 * s2) >> FX_COEFF_Q;
    int64_t p = s1 * s1 + s2 * s2 - cross;

    return (p < 0) ? 0u : (uint64_t)p;
}

uint32_t gz_magnitude(const gz_t *g)
{
    return fx_isqrt64(gz_power(g));
}
