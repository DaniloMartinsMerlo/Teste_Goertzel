/* goertzel.h -- DFT de um unico bin pela recorrencia de Goertzel.
 *
 * Referencia: ST AN5647, "The Goertzel algorithm to compute individual
 * terms of the DFT in STM32 products".
 *
 * A recorrencia eh
 *
 *     s[n] = x[n] + 2*cos(w)*s[n-1] - s[n-2],      w = 2*pi*k/N
 *
 * e, depois de N amostras, com s1 = s[N-1] e s2 = s[N-2]:
 *
 *     Y  = s1 - e^(-jw) * s2   =   X_k * e^(-jw)
 *     |Y| = |X_k|   (exato, verificado contra DFT em double)
 *
 * Ou seja: o fasor devolvido por gz_phasor() eh o X_k da DFT a menos de
 * uma rotacao FIXA de e^(-jw). Como w eh constante, essa rotacao eh a
 * mesma em todo bloco -- ela nao afeta magnitude nenhuma e, na fase, eh
 * um offset constante que a calibracao de crosstalk absorve. Por isso nao
 * gastamos ciclos desfazendo ela.
 *
 * Custo: 1 multiplicacao + 2 somas por amostra por bin.
 *
 * HEADROOM (por que int64 no produto)
 * -----------------------------------
 * O ressonador de Goertzel tem polos SOBRE o circulo unitario: ele eh
 * marginalmente estavel e |s| cresce linearmente com n quando o sinal esta
 * exatamente no bin. Para um seno de fundo de escala:
 *
 *     |s|max ~= (N/2) * A = 150 * 2048 = 307200 ~= 2^18.2
 *
 * Com coeff em Q14, |coeff| <= 32768 = 2^15, logo o produto coeff*s1 chega
 * a 2^33 -- NAO cabe em int32. Por isso o produto eh int64. Nao eh
 * concessao: em Cortex-M4 isso eh SMULL, 1 ciclo, e o orcamento total do
 * DSP fica em ~0.5% de CPU (ver docs/DSP.md, "Orcamento de CPU"). Em troca,
 * Q14 da 8x mais precisao de fase que Q11 -- e a fase eh o que sustenta a
 * subtracao vetorial do crosstalk.
 *
 * Se um dia isto tiver de rodar em Cortex-M0 sem multiplicador de 64 bits,
 * baixe COEFF_Q para 11 no gerador: o produto passa a caber em int32 com
 * 1 bit de margem, ao custo de ~0.01 rad de erro de fase.
 */

#ifndef GOERTZEL_H
#define GOERTZEL_H

#include <stdint.h>
#include "fx_math.h"

typedef struct {
    int32_t  coeff;   /* 2*cos(w) em Q11                        */
    int32_t  cos_q15;     /* cos(w) em Q15                          */
    int32_t  sin_q15;     /* sin(w) em Q15                          */
    int32_t  s1;          /* s[n-1]                                 */
    int32_t  s2;          /* s[n-2]                                 */
    uint32_t peak;        /* max|s| do bloco -- diagnostico/overflow */
    uint16_t bin;
} gz_t;

/* Inicializa para o bin k (0 .. N/2). Le os coeficientes de fx_tables.h;
 * nao faz trigonometria em runtime. */
void gz_init(gz_t *g, uint16_t bin);

/* Zera o estado. Chamar no inicio de cada bloco. */
void gz_reset(gz_t *g);

/* Uma amostra. x deve estar com a componente CC ja removida. */
void gz_push(gz_t *g, int32_t x);

/* Processa um bloco inteiro (mais rapido: mantem s1/s2 em registrador).
 * Nao chama gz_reset() -- o chamador decide. */
void gz_push_block(gz_t *g, const int16_t *x, uint16_t n);

/* Fasor Y = s1 - e^(-jw)*s2. Ver a nota de rotacao no topo do arquivo. */
fx_cplx_t gz_phasor(const gz_t *g);

/* |X_k|^2 = s1^2 + s2^2 - 2*cos(w)*s1*s2.
 * Caminho barato quando so a energia interessa (ETAPA B): dispensa
 * completamente cos/sin na extracao. */
uint64_t gz_power(const gz_t *g);

/* |X_k|, i.e. sqrt(gz_power()). */
uint32_t gz_magnitude(const gz_t *g);

#endif /* GOERTZEL_H */
