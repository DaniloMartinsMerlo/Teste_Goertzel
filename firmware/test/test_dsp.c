/* test_dsp.c -- valida o nucleo de DSP contra a matematica exata.
 *
 * A referencia nao eh "outro jeito de escrever a mesma coisa": eh uma DFT
 * de forca bruta em double. Se o Goertzel de ponto fixo concorda com ela,
 * a recorrencia, o Q dos coeficientes, a extracao de Re/Im e o headroom
 * estao todos certos ao mesmo tempo.
 */

#include "test_util.h"
#include "../core/dsp/fx_math.h"
#include "../core/dsp/goertzel.h"
#include "../core/dsp/lockin.h"
#include "../core/config/sensor_config.h"

#include <string.h>

#define N   CFG_BLOCK_N
#define FS  ((double)CFG_SAMPLE_RATE_HZ)

/* DFT direta no bin k, em double. */
static void dft_bin(const int16_t *x, uint16_t n, uint16_t k,
                    double *re, double *im)
{
    double sr = 0.0, si = 0.0;
    for (uint16_t i = 0u; i < n; i++) {
        double w = 2.0 * M_PI * (double)k * (double)i / (double)n;
        sr += (double)x[i] * cos(w);
        si -= (double)x[i] * sin(w);
    }
    *re = sr;
    *im = si;
}

static void fill_tone(int16_t *x, uint16_t n, double freq_hz,
                      double amp, double phase)
{
    for (uint16_t i = 0u; i < n; i++) {
        double v = amp * cos(2.0 * M_PI * freq_hz * (double)i / FS + phase);
        long c = (long)(v >= 0.0 ? v + 0.5 : v - 0.5);
        if (c >  32767) c =  32767;
        if (c < -32768) c = -32768;
        x[i] = (int16_t)c;
    }
}

/* ---------------------------------------------------------------------- */

static void test_isqrt(void)
{
    SECTION("fx_isqrt64");

    CHECK(fx_isqrt64(0) == 0, "sqrt(0)");
    CHECK(fx_isqrt64(1) == 1, "sqrt(1)");
    CHECK(fx_isqrt64(2) == 1, "sqrt(2) trunca para 1");
    CHECK(fx_isqrt64(4) == 2, "sqrt(4)");
    CHECK(fx_isqrt64(9999) == 99, "sqrt(9999)=99");
    CHECK(fx_isqrt64(10000) == 100, "sqrt(10000)");

    /* Quadrados perfeitos exatos em toda a faixa, incluindo o topo. */
    for (uint64_t r = 1u; r < (1ull << 31); r = r * 3u + 1u) {
        CHECK(fx_isqrt64(r * r) == r, "sqrt(%llu^2)", (unsigned long long)r);
        CHECK(fx_isqrt64(r * r - 1u) == r - 1u,
              "sqrt(%llu^2-1)", (unsigned long long)r);
    }

    /* Concorda com o sqrt de double em valores aleatorios. */
    uint64_t s = 12345u;
    for (int i = 0; i < 20000; i++) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t v = s >> 20;   /* ate ~2^44 */
        uint32_t got = fx_isqrt64(v);
        uint64_t want = (uint64_t)sqrt((double)v);
        /* Ajusta a referencia de double para o piso exato. */
        while (want * want > v)             want--;
        while ((want + 1u) * (want + 1u) <= v) want++;
        CHECK(got == want, "sqrt(%llu): got %u want %llu",
              (unsigned long long)v, got, (unsigned long long)want);
        if (got != want) break;   /* nao inunda a saida se quebrar */
    }
}

static void test_atan2(void)
{
    SECTION("fx_atan2_q15");

    double worst = 0.0;
    for (int deg = -179; deg <= 180; deg++) {
        double rad = (double)deg * M_PI / 180.0;
        int32_t re = (int32_t)(100000.0 * cos(rad));
        int32_t im = (int32_t)(100000.0 * sin(rad));

        int32_t got = fx_atan2_q15(im, re);
        double got_rad = (double)got / 32768.0;

        double err = fabs(got_rad - rad);
        if (err > M_PI) {
            err = fabs(err - 2.0 * M_PI);
        }
        if (err > worst) {
            worst = err;
        }
    }
    printf("  erro maximo de fase: %.6f rad (%.4f deg)\n",
           worst, worst * 180.0 / M_PI);
    CHECK(worst < 0.002, "atan2 dentro de 0.002 rad");

    CHECK(fx_atan2_q15(0, 0) == 0, "atan2(0,0) = 0");
    CHECK(fx_atan2_q15(0, 1000) == 0, "atan2(0,+) = 0");
}

static void test_goertzel_vs_dft(void)
{
    SECTION("Goertzel vs DFT de forca bruta");

    static int16_t x[N];
    const uint16_t bins[] = { 38u, 56u, 90u, 140u };

    for (unsigned b = 0u; b < sizeof(bins) / sizeof(bins[0]); b++) {
        uint16_t k = bins[b];
        double freq = (double)k * CFG_BIN_HZ;

        fill_tone(x, N, freq, 1500.0, 0.4);

        gz_t g;
        gz_init(&g, k);
        gz_push_block(&g, x, N);

        double dre, dim;
        dft_bin(x, N, k, &dre, &dim);
        double dmag = sqrt(dre * dre + dim * dim);

        uint32_t gmag = gz_magnitude(&g);

        /* Magnitude: o erro vem so da quantizacao Q11 do coeficiente. */
        CHECK_NEAR((double)gmag, dmag, dmag * 0.002 + 4.0,
                   "bin %u magnitude", k);

        /* Amplitude recuperada: |X| = A*N/2 para um cosseno. */
        double amp = 2.0 * (double)gmag / (double)N;
        CHECK_NEAR(amp, 1500.0, 8.0, "bin %u amplitude recuperada", k);

        /* Fase: o fasor de gz_phasor() eh X_k girado de e^(-jw). Checa
         * que a rotacao eh EXATAMENTE essa (nao "mais ou menos"). */
        fx_cplx_t z = gz_phasor(&g);
        double gph = (double)fx_atan2_q15(z.im, z.re) / 32768.0;
        double dph = atan2(dim, dre);
        double w   = 2.0 * M_PI * (double)k / (double)N;
        double diff = gph - (dph - w);
        while (diff >  M_PI) diff -= 2.0 * M_PI;
        while (diff < -M_PI) diff += 2.0 * M_PI;
        CHECK_NEAR(diff, 0.0, 0.01, "bin %u: arg(Y) == arg(X_k) - w", k);
    }
}

static void test_goertzel_selectivity(void)
{
    SECTION("Seletividade: bin da portadora vs interferentes");

    static int16_t x[N];
    gz_t carrier;
    gz_init(&carrier, CFG_CARRIER_BIN);

    /* Tom na portadora: referencia. */
    fill_tone(x, N, CFG_CARRIER_HZ, 1000.0, 0.0);
    gz_reset(&carrier);
    gz_push_block(&carrier, x, N);
    double ref = (double)gz_magnitude(&carrier);
    CHECK(ref > 0.0, "referencia nao nula");

    struct { double hz; const char *nome; double min_db; } casos[] = {
        {     0.0, "CC",             60.0 },
        {   100.0, "ripple de rede", 55.0 },
        { 38000.0, "TSOP 38 kHz",    40.0 },
        { 56000.0, "TSOP 56 kHz",    40.0 },
        { 89000.0, "1 bin ao lado",  25.0 },
    };

    for (unsigned i = 0u; i < sizeof(casos) / sizeof(casos[0]); i++) {
        if (casos[i].hz == 0.0) {
            for (uint16_t j = 0u; j < N; j++) x[j] = 1000;
        } else {
            fill_tone(x, N, casos[i].hz, 1000.0, 0.3);
        }
        gz_reset(&carrier);
        gz_push_block(&carrier, x, N);
        double m = (double)gz_magnitude(&carrier);
        double rej = db(ref / (m > 0.0 ? m : 1e-9));
        printf("  rejeicao de %-16s : %6.1f dB\n", casos[i].nome, rej);
        CHECK(rej >= casos[i].min_db, "%s rejeitado >= %.0f dB (deu %.1f)",
              casos[i].nome, casos[i].min_db, rej);
    }
}

static void test_lockin_square_ref(void)
{
    SECTION("Lock-in de referencia +-1 (a variante do plano)");

    lockin_sq_t sq;
    bool ok = lockin_sq_init(&sq, CFG_CARRIER_BIN);
    CHECK(ok, "lockin_sq_init para bin %u", CFG_CARRIER_BIN);
    printf("  periodo da referencia: %u amostras\n", sq.period);
    CHECK(sq.period == 10u, "periodo = N/gcd(90,300) = 10");

    /* Padrao esperado, derivado a mao a partir de cos(0.6*pi*n). */
    const int8_t want_i[10] = { 1, -1, -1,  1,  1, -1,  1,  1, -1, -1 };
    const int8_t want_q[10] = { 0, -1,  1,  1, -1,  0,  1, -1, -1,  1 };
    for (int i = 0; i < 10; i++) {
        CHECK(sq.ref_i[i] == want_i[i], "ref_i[%d] = %d (deu %d)",
              i, want_i[i], sq.ref_i[i]);
        CHECK(sq.ref_q[i] == want_q[i], "ref_q[%d] = %d (deu %d)",
              i, want_q[i], sq.ref_q[i]);
    }

    /* Deve responder ao tom na portadora e rejeitar CC. */
    static int16_t x[N];

    fill_tone(x, N, CFG_CARRIER_HZ, 1000.0, 0.0);
    lockin_sq_reset(&sq);
    lockin_sq_push_block(&sq, x, N);
    double on = (double)fx_cplx_abs(lockin_sq_phasor(&sq));
    CHECK(on > 0.0, "responde a portadora");

    for (uint16_t j = 0u; j < N; j++) x[j] = 1000;
    lockin_sq_reset(&sq);
    lockin_sq_push_block(&sq, x, N);
    double dc = (double)fx_cplx_abs(lockin_sq_phasor(&sq));
    printf("  rejeicao de CC: %.1f dB\n", db(on / (dc > 0 ? dc : 1e-9)));
    CHECK(db(on / (dc > 0 ? dc : 1e-9)) > 50.0, "referencia +-1 rejeita CC");

    /* O ponto tecnico que justifica o default ser o Goertzel: a referencia
     * quadrada correlaciona com a 3a harmonica; a senoidal, nao. */
    fill_tone(x, N, 3.0 * CFG_CARRIER_HZ, 1000.0, 0.0);

    lockin_sq_reset(&sq);
    lockin_sq_push_block(&sq, x, N);
    double sq_h3 = (double)fx_cplx_abs(lockin_sq_phasor(&sq));

    gz_t g;
    gz_init(&g, CFG_CARRIER_BIN);
    gz_push_block(&g, x, N);
    double gz_h3 = (double)gz_magnitude(&g);

    fill_tone(x, N, CFG_CARRIER_HZ, 1000.0, 0.0);
    gz_reset(&g);
    gz_push_block(&g, x, N);
    double gz_ref = (double)gz_magnitude(&g);

    printf("  vazamento da 3a harmonica (270 kHz -> alias 30 kHz):\n");
    printf("    ref +-1     : %7.1f dB abaixo do fundamental\n",
           db(on / (sq_h3 > 0 ? sq_h3 : 1e-9)));
    printf("    ref senoidal: %7.1f dB abaixo do fundamental\n",
           db(gz_ref / (gz_h3 > 0 ? gz_h3 : 1e-9)));
    CHECK(gz_h3 < sq_h3 || gz_h3 < 1.0,
          "referencia senoidal rejeita a 3a harmonica melhor que a +-1");
}

static void test_ewma(void)
{
    SECTION("fx_ewma");

    fx_ewma_t f;
    fx_ewma_init(&f, 3u);

    /* Priming: a primeira amostra deve sair inteira, nao 1/8 dela. Sem
     * isso o sensor comeca a luta reportando "vazio". */
    CHECK(fx_ewma_push(&f, 1000) == 1000, "primeira amostra prima o filtro");

    for (int i = 0; i < 200; i++) {
        fx_ewma_push(&f, 1000);
    }
    CHECK_NEAR(fx_ewma_get(&f), 1000, 8, "converge para o valor CC");

    /* Degrau para baixo: deve cair de forma monotona, sem overshoot. */
    int32_t prev = fx_ewma_get(&f);
    for (int i = 0; i < 50; i++) {
        int32_t v = fx_ewma_push(&f, 0);
        CHECK(v <= prev, "monotono na descida (passo %d)", i);
        if (v > prev) break;   /* nao inunda a saida se quebrar */
        prev = v;
    }
    CHECK_NEAR(fx_ewma_get(&f), 0, 8, "converge para zero");

    /* Media de ruido de media zero deve tender a zero. */
    fx_ewma_init(&f, 5u);
    uint64_t s = 999u;
    for (int i = 0; i < 5000; i++) {
        s = s * 6364136223846793005ull + 1u;
        int32_t n = (int32_t)((s >> 40) % 2001u) - 1000;
        fx_ewma_push(&f, n);
    }
    CHECK(labs((long)fx_ewma_get(&f)) < 200,
          "ruido de media zero converge para ~0 (deu %d)", fx_ewma_get(&f));
}

int main(void)
{
    printf("Plano: Fs=%u Hz  N=%u  df=%u Hz  bloco=%.3f ms  f0=%u Hz (bin %u)\n",
           CFG_SAMPLE_RATE_HZ, CFG_BLOCK_N, CFG_BIN_HZ,
           1000.0 * CFG_BLOCK_N / CFG_SAMPLE_RATE_HZ,
           CFG_CARRIER_HZ, CFG_CARRIER_BIN);

    test_isqrt();
    test_atan2();
    test_goertzel_vs_dft();
    test_goertzel_selectivity();
    test_lockin_square_ref();
    test_ewma();

    return test_report("test_dsp");
}
