#include "sim_frontend.h"
#include "../../core/config/sensor_config.h"

#include <math.h>
#include <string.h>

#define TWO_PI  6.283185307179586

void sim_init(sim_frontend_t *f, uint64_t seed)
{
    memset(f, 0, sizeof(*f));

    f->target_dist_cm  = 0.0;
    f->target_reflect  = 0.8;
    f->emitter_on      = false;

    f->pga_gain        = 16.0;
    f->crosstalk_amp   = 25.0;
    f->crosstalk_phase = 0.7;
    f->ambient_dc      = 0.0;
    f->ambient_ripple  = 0.0;
    f->noise_rms       = 8.0;
    f->aaf_pole_hz     = 130000.0;
    f->emitter_duty    = (double)CFG_EMITTER_DUTY_PCT / 100.0;

    f->sample_index = 0u;
    f->rng = seed ? seed : 0x9E3779B97F4A7C15ull;
}

/* xorshift64* -- deterministico e portavel, para o teste ser reproduzivel. */
static double urand(sim_frontend_t *f)
{
    uint64_t x = f->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    f->rng = x;
    return (double)((x * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0;
}

/* Box-Muller, uma amostra por chamada (a segunda eh descartada: custo
 * irrelevante num simulador de teste). */
static double gauss(sim_frontend_t *f)
{
    double u1 = urand(f);
    double u2 = urand(f);
    if (u1 < 1e-12) {
        u1 = 1e-12;
    }
    return sqrt(-2.0 * log(u1)) * cos(TWO_PI * u2);
}

double sim_expected_carrier_amplitude(const sim_frontend_t *f)
{
    if (f->target_dist_cm <= 0.0) {
        return 0.0;
    }

    /* Alvo difuso: ida (1/d^2) e volta (1/d^2) => 1/d^4. Normalizado para
     * uma amplitude de referencia arbitraria a 10 cm. */
    const double ref_cm  = 10.0;
    const double ref_amp = 900.0;
    double d = f->target_dist_cm / ref_cm;
    double a = ref_amp * f->target_reflect / (d * d * d * d);

    /* Amplitude do FUNDAMENTAL de um trem de pulsos de duty d:
     * 2*sin(pi*d)/(pi*d) relativo ao valor medio; aqui normalizamos
     * relativo a duty 50%. */
    double duty = f->emitter_duty;
    double fund = sin(M_PI * duty) / (M_PI * duty);

    return a * fund * (f->pga_gain / 16.0);
}

/* Atenuacao de 1 polo do anti-aliasing em f. */
static double aaf(const sim_frontend_t *f, double hz)
{
    double r = hz / f->aaf_pole_hz;
    /* 2 polos: o projeto pede Sallen-Key de 2a ordem. */
    return 1.0 / (1.0 + r * r);
}

void sim_fill_block(sim_frontend_t *f, uint16_t *out, uint16_t n)
{
    const double fs = (double)CFG_SAMPLE_RATE_HZ;
    const double f0 = (double)CFG_CARRIER_HZ;
    const double duty = f->emitter_duty;

    /* Amplitude do reflexo, sem o fator de duty (aplicado por harmonica). */
    double refl = 0.0;
    if (f->target_dist_cm > 0.0) {
        const double ref_cm = 10.0, ref_amp = 900.0;
        double d = f->target_dist_cm / ref_cm;
        refl = ref_amp * f->target_reflect / (d * d * d * d)
             * (f->pga_gain / 16.0);
    }

    /* Fase do adversario: aleatoria por bloco. Eh isso que o torna
     * incoerente e que obriga a ETAPA B a promediar POTENCIA. */
    double ph38 = urand(f) * TWO_PI;
    double ph56 = urand(f) * TWO_PI;

    /* O jammer na nossa f0 tem fase FIXA de proposito: eh o caso que o
     * chopping deve cancelar. Um jammer de fase aleatoria por bloco seria
     * cancelado pela EWMA de qualquer jeito, o que nao testaria nada. */
    const double phj = 1.9;

    for (uint16_t i = 0u; i < n; i++) {
        double t = (double)f->sample_index / fs;
        double v = 0.0;

        /* --- luz ambiente ---------------------------------------------- */
        v += f->ambient_dc;
        if (f->ambient_ripple != 0.0) {
            v += f->ambient_ripple * sin(TWO_PI * 100.0 * t);
        }

        /* --- nosso emissor: fundamental + harmonicas impares ----------- */
        if (f->emitter_on) {
            for (int h = 1; h <= 11; h += 2) {
                double fh = f0 * (double)h;
                /* Coeficiente de Fourier de um trem de pulsos de duty d. */
                double ch = fabs(sin(M_PI * (double)h * duty))
                          / (M_PI * (double)h * duty);
                double att = aaf(f, fh);

                /* Reflexo do alvo: fase proporcional ao caminho optico. */
                if (refl != 0.0) {
                    double phi = 0.9 + 0.02 * f->target_dist_cm;
                    v += refl * ch * att
                       * cos(TWO_PI * fh * t + (double)h * phi);
                }

                /* Crosstalk interno: caminho fixo, fase fixa, independente
                 * do alvo. Eh o termo que a calibracao complexa cancela. */
                v += f->crosstalk_amp * ch * att
                   * cos(TWO_PI * fh * t + (double)h * f->crosstalk_phase);
            }
        }

        /* --- emissores adversarios ------------------------------------- */
        if (f->enemy_38k_amp != 0.0) {
            v += f->enemy_38k_amp * aaf(f, 38000.0)
               * cos(TWO_PI * 38000.0 * t + ph38);
        }
        if (f->enemy_56k_amp != 0.0) {
            v += f->enemy_56k_amp * aaf(f, 56000.0)
               * cos(TWO_PI * 56000.0 * t + ph56);
        }
        if (f->jammer_f0_amp != 0.0) {
            v += f->jammer_f0_amp * aaf(f, f0) * cos(TWO_PI * f0 * t + phj);
        }

        /* --- ruido ------------------------------------------------------ */
        v += f->noise_rms * gauss(f);

        /* --- ADC de 12 bits, centrado em VREF/2 ------------------------ */
        long code = (long)((double)CFG_DC_NOMINAL + v + 0.5);
        if (code < 0)    code = 0;
        if (code > 4095) code = 4095;
        out[i] = (uint16_t)code;

        f->sample_index++;
    }
}
