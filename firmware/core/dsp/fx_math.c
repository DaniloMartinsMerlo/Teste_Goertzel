#include "fx_math.h"

/* ---------------------------------------------------------------------- */

uint32_t fx_isqrt64(uint64_t v)
{
    if (v == 0u) {
        return 0u;
    }

    /* Semente: 2^ceil(bits/2) >= sqrt(v). Garante convergencia monotona
     * decrescente do Newton inteiro, sem oscilar. */
    unsigned bits = 0u;
    uint64_t t = v;
    while (t != 0u) {
        t >>= 1;
        bits++;
    }

    uint64_t x = (uint64_t)1u << ((bits + 1u) / 2u);

    for (;;) {
        uint64_t nx = (x + v / x) / 2u;
        if (nx >= x) {
            break;
        }
        x = nx;
    }

    /* Newton inteiro pode parar um acima do piso; corrige. */
    while (x != 0u && x * x > v) {
        x--;
    }

    return (x > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)x;
}

uint64_t fx_cplx_abs2(fx_cplx_t z)
{
    int64_t re = (int64_t)z.re;
    int64_t im = (int64_t)z.im;
    return (uint64_t)(re * re) + (uint64_t)(im * im);
}

uint32_t fx_cplx_abs(fx_cplx_t z)
{
    return fx_isqrt64(fx_cplx_abs2(z));
}

/* ---------------------------------------------------------------------- */

/* atan(a) para a em [0,1], entrada e saida em Q15.
 *
 *   atan(a) ~= a*(pi/4) + a*(1-a)*(0.2447 + 0.0663*a)
 *
 * Aproximacao classica (Rajan et al.), erro maximo 0.0015 rad. Todos os
 * produtos intermediarios cabem em int32:
 *   a <= 32768; a*25736 <= 8.4e8; a*(32768-a) <= 2^28;
 *   (>>15 disso) * 10189 <= 8.4e7.
 */
#define FX_PI_4_Q15   25736   /* pi/4  */
#define FX_C1_Q15      8017   /* 0.2447 */
#define FX_C2_Q15      2172   /* 0.0663 */

static int32_t fx_atan_unit_q15(int32_t a)
{
    int32_t lin  = (a * FX_PI_4_Q15) >> 15;
    int32_t bow  = (a * (32768 - a)) >> 15;
    int32_t corr = (bow * (FX_C1_Q15 + ((FX_C2_Q15 * a) >> 15))) >> 15;
    return lin + corr;
}

#define FX_PI_Q15     102944
#define FX_PI_2_Q15    51472

int32_t fx_atan2_q15(int32_t im, int32_t re)
{
    if (re == 0 && im == 0) {
        return 0;
    }

    /* Trabalha no primeiro octante e depois espelha. O ratio precisa de
     * 64 bits porque |min| << 15 pode passar de int32. */
    int32_t ax = (re < 0) ? -re : re;
    int32_t ay = (im < 0) ? -im : im;

    int32_t mn = (ax < ay) ? ax : ay;
    int32_t mx = (ax < ay) ? ay : ax;

    int32_t ratio = (int32_t)(((int64_t)mn << 15) / mx);
    int32_t ang   = fx_atan_unit_q15(ratio);

    if (ay > ax) {
        ang = FX_PI_2_Q15 - ang;   /* reflete sobre a diagonal */
    }
    if (re < 0) {
        ang = FX_PI_Q15 - ang;     /* 2o/3o quadrante */
    }
    if (im < 0) {
        ang = -ang;                /* 3o/4o quadrante */
    }

    return ang;
}

/* ---------------------------------------------------------------------- */

uint16_t fx_div_q8(uint32_t num, uint32_t den)
{
    if (den == 0u) {
        return 0xFFFFu;
    }

    /* num << 8 pode estourar 32 bits, por isso o 64. */
    uint64_t q = ((uint64_t)num << 8) / den;
    return (q > 0xFFFFu) ? 0xFFFFu : (uint16_t)q;
}

/* ---------------------------------------------------------------------- */

void fx_ewma_init(fx_ewma_t *f, uint8_t shift)
{
    f->acc     = 0;
    f->shift   = shift;
    f->primed  = 0u;
}

int32_t fx_ewma_push(fx_ewma_t *f, int32_t x)
{
    if (!f->primed) {
        /* Sem isto o filtro sobe do zero e a primeira leitura sai baixa
         * por 2^shift ciclos -- o que, num sensor de combate, eh um
         * "nao vi ninguem" na largada. */
        f->acc    = x << f->shift;
        f->primed = 1u;
    } else {
        f->acc += x - (f->acc >> f->shift);
    }
    return f->acc >> f->shift;
}

int32_t fx_ewma_get(const fx_ewma_t *f)
{
    return f->acc >> f->shift;
}
