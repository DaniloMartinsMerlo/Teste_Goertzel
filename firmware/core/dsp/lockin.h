/* lockin.h -- amplificador lock-in digital.
 *
 * O QUE EH LOCK-IN E O QUE EH GOERTZEL, AQUI
 * ------------------------------------------
 * Matematicamente os dois sao a mesma coisa: correlacao do sinal com uma
 * referencia na frequencia de interesse. A diferenca esta no papel:
 *
 *   Goertzel  = o MISTURADOR + integrador de UM bloco. Devolve o fasor do
 *               bloco (I/Q). Sozinho, integra 1 ms -> ENBW = 1 kHz.
 *
 *   Lock-in   = a cadeia completa e travada em fase: misturador coerente
 *               com a referencia do emissor, chopping (ON - OFF), subtracao
 *               vetorial do crosstalk e filtro passa-baixa LONGO sobre I/Q
 *               ao longo de muitos blocos -> ENBW de ~16 Hz.
 *
 * O ganho de processamento vem do filtro pos-deteccao, nao do Goertzel:
 *
 *     de ~150 kHz de banda analogica para ~16 Hz  =>  ~39 dB de SNR
 *
 * POR QUE A ACUMULACAO COERENTE ENTRE BLOCOS EH LEGAL
 * ---------------------------------------------------
 * Somar I/Q de blocos diferentes so faz sentido se a fase da portadora for
 * a MESMA no inicio de cada bloco. Isso vale porque:
 *
 *   1. O timer da portadora e o timer que dispara o ADC saem do MESMO
 *      clock (144 MHz) com ARR inteiros (1600 e 480) -> nao existe deriva.
 *   2. N*f0/Fs = 300*90000/300000 = 90 ciclos INTEIROS por bloco -> o
 *      bloco termina exatamente na mesma fase em que comecou.
 *
 * Se qualquer um dos dois deixar de valer (N mudou, Fs mudou, f0 mudou),
 * a EWMA sobre I/Q passa a somar fasores girando e a magnitude despenca.
 * Os _Static_assert em goertzel.c e a checagem em lockin_init() cobrem isso.
 *
 * A VARIANTE +-1 (o que o plano descreve)
 * ---------------------------------------
 * O plano fala em "multiplicar por +1 ou -1 e tirar a media". Isso eh
 * lock-in com referencia QUADRADA: em vez de multiplicar por cos(wn), se
 * multiplica por sign(cos(wn)), que so precisa de somas. Esta implementado
 * em lockin_sq_* e testado.
 *
 * A diferenca pratica: a referencia quadrada tambem correlaciona com as
 * harmonicas IMPARES de f0 (3*f0, 5*f0, ...), com peso 1/3, 1/5, ...
 * Como o nosso emissor eh PWM 10% (rico em harmonicas), a referencia
 * senoidal do Goertzel eh mais limpa. Num Cortex-M4F a 144 MHz o custo do
 * Goertzel eh ~0.2% de CPU, entao o default eh o Goertzel e o +-1 fica
 * disponivel para MCU sem multiplicador. Ver docs/DSP.md.
 */

#ifndef LOCKIN_H
#define LOCKIN_H

#include <stdint.h>
#include <stdbool.h>
#include "fx_math.h"

/* ---------------------------------------------------------------------- */
/* Cadeia pos-deteccao: chopping + crosstalk + EWMA complexa               */
/* ---------------------------------------------------------------------- */

typedef struct {
    /* Filtro passa-baixa do lock-in, um polo por eixo. */
    fx_ewma_t lpf_i;
    fx_ewma_t lpf_q;

    /* Crosstalk interno LED->fotodiodo, medido na calibracao.
     *
     * Detalhe que importa: esse vazamento eh COERENTE (mesma fase todo
     * bloco, porque o caminho optico/eletrico eh fixo). Logo ele tem de
     * ser subtraido no PLANO COMPLEXO, nao em magnitude. Subtrair em
     * magnitude destroi a informacao de alvo fraco; subtrair vetorialmente
     * cancela o vazamento e preserva o alvo. */
    fx_cplx_t crosstalk;
    bool      crosstalk_valid;

    /* Ultimo resultado. */
    fx_cplx_t raw;        /* ON - OFF, antes do crosstalk   */
    fx_cplx_t corrected;  /* depois do crosstalk            */
    fx_cplx_t filtered;   /* depois da EWMA                 */
    uint32_t  magnitude;  /* |filtered|                     */
    int32_t   phase_q15;  /* arg(filtered), Q15 rad         */
} lockin_t;

void lockin_init(lockin_t *l, uint8_t lpf_shift);

/* Reseta o filtro (ex.: apos trocar o ganho do PGA, que muda a escala). */
void lockin_reset_filter(lockin_t *l);

/* Grava o crosstalk atual como referencia de "sem alvo". */
void lockin_set_crosstalk(lockin_t *l, fx_cplx_t z);
void lockin_clear_crosstalk(lockin_t *l);

/* Um ciclo de chopping completo.
 *   on  = fasor do bloco com emissor ligado
 *   off = fasor do bloco com emissor desligado (fundo na nossa frequencia)
 * Atualiza raw/corrected/filtered/magnitude/phase_q15. */
void lockin_push_cycle(lockin_t *l, fx_cplx_t on, fx_cplx_t off);

/* ---------------------------------------------------------------------- */
/* Lock-in de referencia quadrada (+-1) -- o que o plano descreve          */
/* ---------------------------------------------------------------------- */

/* A referencia se fecha em N/gcd(bin,N) amostras -- nao em Fs/f0, que aqui
 * eh 10/3 e nao eh inteiro. Para bin 90 e N = 300: gcd = 30, periodo = 10
 * amostras (= 3 ciclos de portadora). Todos os canais FDMA da lista em
 * sensor_config.h dao periodo <= 25. */
#define LOCKIN_SQ_MAX_PERIOD  32u

typedef struct {
    int8_t   ref_i[LOCKIN_SQ_MAX_PERIOD];  /* sign(cos(wn))  em -1/0/+1 */
    int8_t   ref_q[LOCKIN_SQ_MAX_PERIOD];  /* sign(-sin(wn)) em -1/0/+1 */
    uint16_t period;
    int32_t  acc_i;
    int32_t  acc_q;
    uint16_t phase;   /* 0 .. period-1 */
} lockin_sq_t;

/* Monta o padrao de sinais para o bin dado, a partir de fx_tables.h.
 * Devolve false se o periodo nao couber em LOCKIN_SQ_MAX_PERIOD. */
bool      lockin_sq_init(lockin_sq_t *s, uint16_t bin);
void      lockin_sq_reset(lockin_sq_t *s);
void      lockin_sq_push(lockin_sq_t *s, int32_t x);
void      lockin_sq_push_block(lockin_sq_t *s, const int16_t *x, uint16_t n);
fx_cplx_t lockin_sq_phasor(const lockin_sq_t *s);

#endif /* LOCKIN_H */
