/* sim_frontend.h -- modelo do front-end analogico + cena, para rodar o core
 * no PC sem hardware nenhum.
 *
 * Nao eh enfeite: eh o que permite afinar limiares, medir SNR e testar
 * regressao do DSP antes de a placa existir. O core que roda aqui eh
 * BIT A BIT o mesmo que roda no G4 -- e o mesmo ir_sensor.c.
 *
 * O modelo cobre o que realmente atrapalha na arena:
 *   - reflexo do alvo na portadora propria, com atenuacao ~1/d^4 e fase fixa
 *   - crosstalk interno LED->fotodiodo (coerente, fase fixa, nao depende do alvo)
 *   - emissor adversario em 38/56 kHz, fase aleatoria (incoerente)
 *   - luz ambiente: CC + ripple de rede em 100/120 Hz
 *   - ruido branco gaussiano
 *   - saturacao no trilho do ADC (12 bits)
 *   - harmonicas impares do PWM de duty baixo, com o rolloff do anti-aliasing
 */

#ifndef SIM_FRONTEND_H
#define SIM_FRONTEND_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    /* --- cena ---------------------------------------------------------- */
    double target_dist_cm;     /* 0 = sem alvo                            */
    double target_reflect;     /* 0..1, refletividade (branco ~0.8)       */
    bool   emitter_on;         /* estado do nosso LED neste bloco         */

    double enemy_38k_amp;      /* amplitude do emissor adversario, contagens */
    double enemy_56k_amp;
    double jammer_f0_amp;      /* adversario emitindo NA NOSSA f0          */

    /* --- front-end ----------------------------------------------------- */
    double pga_gain;           /* ganho do PGA                            */
    double crosstalk_amp;      /* vazamento LED->fotodiodo, contagens     */
    double crosstalk_phase;    /* rad                                     */
    double ambient_dc;         /* contagens (offset sobre 2048)           */
    double ambient_ripple;     /* amplitude do ripple de 100 Hz           */
    double noise_rms;          /* contagens rms                           */
    double aaf_pole_hz;        /* polo do filtro anti-aliasing            */
    double emitter_duty;       /* 0..1                                    */

    /* --- estado interno ------------------------------------------------ */
    uint64_t sample_index;
    uint64_t rng;
} sim_frontend_t;

void sim_init(sim_frontend_t *f, uint64_t seed);

/* Gera um bloco de n amostras de ADC de 12 bits. */
void sim_fill_block(sim_frontend_t *f, uint16_t *out, uint16_t n);

/* Amplitude esperada da portadora refletida, em contagens de ADC, para a
 * cena atual. Usada pelos testes para checar o erro do estimador. */
double sim_expected_carrier_amplitude(const sim_frontend_t *f);

#endif /* SIM_FRONTEND_H */
