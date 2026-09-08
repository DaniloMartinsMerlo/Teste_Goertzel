#include "lockin.h"
#include "fx_tables.h"

/* ---------------------------------------------------------------------- */
/* Cadeia pos-deteccao                                                     */
/* ---------------------------------------------------------------------- */

void lockin_init(lockin_t *l, uint8_t lpf_shift)
{
    fx_ewma_init(&l->lpf_i, lpf_shift);
    fx_ewma_init(&l->lpf_q, lpf_shift);

    l->crosstalk.re    = 0;
    l->crosstalk.im    = 0;
    l->crosstalk_valid = false;

    l->raw.re = l->raw.im = 0;
    l->corrected.re = l->corrected.im = 0;
    l->filtered.re = l->filtered.im = 0;
    l->magnitude = 0u;
    l->phase_q15 = 0;
}

void lockin_reset_filter(lockin_t *l)
{
    uint8_t shift = l->lpf_i.shift;
    fx_ewma_init(&l->lpf_i, shift);
    fx_ewma_init(&l->lpf_q, shift);
    l->magnitude = 0u;
}

void lockin_set_crosstalk(lockin_t *l, fx_cplx_t z)
{
    l->crosstalk       = z;
    l->crosstalk_valid = true;
}

void lockin_clear_crosstalk(lockin_t *l)
{
    l->crosstalk.re    = 0;
    l->crosstalk.im    = 0;
    l->crosstalk_valid = false;
}

void lockin_push_cycle(lockin_t *l, fx_cplx_t on, fx_cplx_t off)
{
    /* 1. Chopping: o bloco OFF mede tudo que existe na nossa frequencia
     *    SEM ser nosso -- outro robo em 90 kHz, vazamento de um sensor
     *    vizinho nosso, ruido de rede acoplado. Subtracao vetorial. */
    l->raw.re = on.re - off.re;
    l->raw.im = on.im - off.im;

    /* 2. Crosstalk interno: coerente, logo sai no plano complexo. */
    l->corrected = l->raw;
    if (l->crosstalk_valid) {
        l->corrected.re -= l->crosstalk.re;
        l->corrected.im -= l->crosstalk.im;
    }

    /* 3. Passa-baixa longo sobre I e Q separadamente. Eh AQUI que mora o
     *    ganho de processamento do lock-in. Filtrar I/Q (e nao a
     *    magnitude) eh o que permite a media coerente: ruido de fase
     *    aleatoria se cancela, o alvo (fase fixa) soma. */
    l->filtered.re = fx_ewma_push(&l->lpf_i, l->corrected.re);
    l->filtered.im = fx_ewma_push(&l->lpf_q, l->corrected.im);

    l->magnitude = fx_cplx_abs(l->filtered);
    l->phase_q15 = fx_atan2_q15(l->filtered.im, l->filtered.re);
}

/* ---------------------------------------------------------------------- */
/* Referencia quadrada (+-1)                                               */
/* ---------------------------------------------------------------------- */

static uint16_t gcd_u16(uint16_t a, uint16_t b)
{
    while (b != 0u) {
        uint16_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* cos/sin de 2*pi*idx/N para idx em 0..N-1, a partir da tabela de meio
 * circulo (fx_tables.h guarda so 0..N/2). */
static void unit_circle(uint16_t idx, int32_t *cos_q15, int32_t *sin_q15)
{
    if (idx <= (FX_TABLE_N / 2u)) {
        *cos_q15 =  fx_cos_q15[idx];
        *sin_q15 =  fx_sin_q15[idx];
    } else {
        uint16_t m = (uint16_t)(FX_TABLE_N - idx);
        *cos_q15 =  fx_cos_q15[m];
        *sin_q15 = -fx_sin_q15[m];
    }
}

static int8_t sgn(int32_t v)
{
    /* Zero explicito: a referencia fica de 3 niveis (-1/0/+1) em vez de 2.
     * Isso nao eh detalhe -- uma referencia de 3 niveis rejeita a 3a
     * harmonica muito melhor que uma onda quadrada pura. */
    if (v > 0) return  1;
    if (v < 0) return -1;
    return 0;
}

bool lockin_sq_init(lockin_sq_t *s, uint16_t bin)
{
    uint16_t period = (uint16_t)(FX_TABLE_N / gcd_u16(bin, FX_TABLE_N));
    if (period == 0u || period > LOCKIN_SQ_MAX_PERIOD) {
        s->period = 0u;
        return false;
    }

    s->period = period;

    for (uint16_t n = 0u; n < period; n++) {
        uint16_t idx = (uint16_t)(((uint32_t)bin * n) % FX_TABLE_N);
        int32_t c, sn;
        unit_circle(idx, &c, &sn);

        /* Convencao e^(-jwn): Re usa +cos, Im usa -sin. */
        s->ref_i[n] = sgn(c);
        s->ref_q[n] = sgn(-sn);
    }

    lockin_sq_reset(s);
    return true;
}

void lockin_sq_reset(lockin_sq_t *s)
{
    s->acc_i = 0;
    s->acc_q = 0;
    s->phase = 0u;
}

void lockin_sq_push(lockin_sq_t *s, int32_t x)
{
    s->acc_i += (int32_t)s->ref_i[s->phase] * x;
    s->acc_q += (int32_t)s->ref_q[s->phase] * x;

    if (++s->phase >= s->period) {
        s->phase = 0u;
    }
}

void lockin_sq_push_block(lockin_sq_t *s, const int16_t *x, uint16_t n)
{
    uint16_t phase = s->phase;
    int32_t ai = s->acc_i;
    int32_t aq = s->acc_q;
    const uint16_t period = s->period;

    for (uint16_t i = 0u; i < n; i++) {
        /* Multiplicacao por -1/0/+1: o compilador gera soma/subtracao
         * condicional, sem instrucao de multiplicacao nenhuma. Eh
         * exatamente o "multiplica por +1 ou -1" do plano. */
        const int32_t v = (int32_t)x[i];
        ai += (int32_t)s->ref_i[phase] * v;
        aq += (int32_t)s->ref_q[phase] * v;

        if (++phase >= period) {
            phase = 0u;
        }
    }

    s->phase = phase;
    s->acc_i = ai;
    s->acc_q = aq;
}

fx_cplx_t lockin_sq_phasor(const lockin_sq_t *s)
{
    fx_cplx_t z;
    z.re = s->acc_i;
    z.im = s->acc_q;
    return z;
}
