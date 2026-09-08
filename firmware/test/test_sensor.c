/* test_sensor.c -- roda a maquina de estados completa contra o simulador
 * de front-end. Verifica o comportamento que importa na arena:
 *
 *   - ETAPA A dispara com cegamento e NAO dispara com luz ambiente normal
 *   - ETAPA B acha emissor de 38/56 kHz e identifica qual eh
 *   - ETAPA B nao liga o emissor (furtividade)
 *   - ETAPA C detecta alvo e a proximidade eh monotonica com a distancia
 *   - a calibracao complexa de crosstalk cancela o vazamento interno
 *   - o chopping mata interferencia estatica na nossa propria frequencia
 *   - a cascata respeita a prioridade A > B > C do plano
 */

#include "test_util.h"
#include "../core/sensor/ir_sensor.h"
#include "../platform/host/sim_frontend.h"

#include <string.h>

/* --- duble dos ports --------------------------------------------------- */

typedef struct {
    sim_frontend_t *sim;
    bool     emitter_on;
    uint32_t emitter_on_blocks;
    uint8_t  gain_index;
    uint32_t now_us;
} harness_t;

static const double k_gain_table[CFG_PGA_GAIN_COUNT] = { 2.0, 8.0, 16.0, 32.0 };

static void h_emitter_set(void *ctx, bool on)
{
    harness_t *h = (harness_t *)ctx;
    h->emitter_on = on;
}

static void h_pga_set_gain(void *ctx, uint8_t idx)
{
    harness_t *h = (harness_t *)ctx;
    if (idx >= CFG_PGA_GAIN_COUNT) {
        idx = CFG_PGA_GAIN_COUNT - 1u;
    }
    h->gain_index = idx;
    h->sim->pga_gain = k_gain_table[idx];
}

static uint32_t h_now_us(void *ctx)
{
    harness_t *h = (harness_t *)ctx;
    h->now_us += 1000u;
    return h->now_us;
}

static void harness_ports(harness_t *h, ports_t *p)
{
    memset(p, 0, sizeof(*p));
    p->emitter.set     = h_emitter_set;
    p->emitter.ctx     = h;
    p->pga.set_gain    = h_pga_set_gain;
    p->pga.ctx         = h;
    p->time.now_us     = h_now_us;
    p->time.ctx        = h;
    p->log.write       = NULL;
}

/* Roda `cycles` ciclos de 4 blocos, alimentando o core com blocos do
 * simulador. Espelha exatamente o que o adapter de DMA faz no G4. */
static void run_cycles(ir_sensor_t *s, harness_t *h, unsigned cycles)
{
    static uint16_t buf[2][CFG_BLOCK_N];
    unsigned blocks = cycles * IR_PHASE_COUNT;

    for (unsigned b = 0u; b < blocks; b++) {
        /* O simulador precisa saber se o LED estava aceso DURANTE este
         * bloco -- e o core ja chaveou o emissor no fim do bloco anterior. */
        h->sim->emitter_on = h->emitter_on;
        if (h->emitter_on) {
            h->emitter_on_blocks++;
        }

        uint16_t *slot = buf[b & 1u];
        sim_fill_block(h->sim, slot, CFG_BLOCK_N);

        ir_sensor_on_block_isr(s, slot, CFG_BLOCK_N);
        ir_sensor_process(s);
    }
}

static void setup(ir_sensor_t *s, harness_t *h, sim_frontend_t *sim,
                  uint64_t seed)
{
    sim_init(sim, seed);
    memset(h, 0, sizeof(*h));
    h->sim = sim;
    h->gain_index = CFG_PGA_GAIN_DEFAULT;
    sim->pga_gain = k_gain_table[CFG_PGA_GAIN_DEFAULT];

    ports_t p;
    harness_ports(h, &p);
    ir_sensor_init(s, &p);
}

/* Calibra o crosstalk com a cena vazia (como se faz na bancada). */
static void calibrate(ir_sensor_t *s, harness_t *h)
{
    double saved = h->sim->target_dist_cm;
    h->sim->target_dist_cm = 0.0;

    ir_sensor_calibrate(s);
    run_cycles(s, h, 40u);

    CHECK((ir_sensor_result(s)->flags & IR_FLAG_NO_CROSSTALK) == 0u,
          "calibracao de crosstalk concluida");

    h->sim->target_dist_cm = saved;
}

/* ---------------------------------------------------------------------- */

static void test_stage_a_blinding(void)
{
    SECTION("ETAPA A -- cegamento");

    ir_sensor_t s;
    harness_t h;
    sim_frontend_t sim;

    /* Luz ambiente normal (sol indireto): NAO deve reportar cegamento. */
    setup(&s, &h, &sim, 1u);
    sim.ambient_dc     = 300.0;
    sim.ambient_ripple = 40.0;
    run_cycles(&s, &h, 30u);
    CHECK(ir_sensor_result(&s)->verdict != IR_VERDICT_BLINDED,
          "luz ambiente normal nao eh cegamento (veredito %d, dc %u)",
          ir_sensor_result(&s)->verdict, ir_sensor_result(&s)->dc_mean);

    /* Alguem jogando IR forte: DEVE reportar cegamento, e reportar antes
     * de qualquer outra coisa. */
    setup(&s, &h, &sim, 2u);
    sim.ambient_dc = 1600.0;
    run_cycles(&s, &h, 30u);
    const ir_result_t *r = ir_sensor_result(&s);
    CHECK(r->verdict == IR_VERDICT_BLINDED,
          "IR forte = cegamento (veredito %d, dc %u, flags 0x%02x)",
          r->verdict, r->dc_mean, r->flags);
    CHECK((r->flags & IR_FLAG_DC_HIGH) != 0u, "flag DC_HIGH marcada");
}

static void test_stage_b_threat(void)
{
    SECTION("ETAPA B -- escuta passiva de 38/56 kHz");

    ir_sensor_t s;
    harness_t h;
    sim_frontend_t sim;

    /* Sem adversario: nenhuma ameaca. */
    setup(&s, &h, &sim, 3u);
    run_cycles(&s, &h, 40u);
    CHECK(!ir_sensor_result(&s)->threat_any,
          "sem adversario, sem ameaca (mask 0x%02x)",
          ir_sensor_result(&s)->threat_mask);

    /* Adversario com sensor de 38 kHz apontado para nos. */
    setup(&s, &h, &sim, 4u);
    sim.enemy_38k_amp = 120.0;
    run_cycles(&s, &h, 40u);
    const ir_result_t *r = ir_sensor_result(&s);
    CHECK(r->threat_any, "38 kHz detectado");
    CHECK(r->verdict == IR_VERDICT_ENEMY_GOERTZEL,
          "veredito = INIMIGO via Goertzel (deu %d)", r->verdict);
    CHECK(r->threat_freq_khz == 38u,
          "identifica a frequencia como 38 kHz (deu %u)", r->threat_freq_khz);

    /* Adversario de 56 kHz. */
    setup(&s, &h, &sim, 5u);
    sim.enemy_56k_amp = 120.0;
    run_cycles(&s, &h, 40u);
    r = ir_sensor_result(&s);
    CHECK(r->threat_any, "56 kHz detectado");
    CHECK(r->threat_freq_khz == 56u,
          "identifica a frequencia como 56 kHz (deu %u)", r->threat_freq_khz);

    /* Sensibilidade: com que amplitude a ETAPA B ainda pega? */
    printf("  limiar de deteccao da ETAPA B (ruido rms = 8 contagens):\n");
    for (double amp = 8.0; amp <= 128.0; amp *= 2.0) {
        setup(&s, &h, &sim, 6u);
        sim.enemy_38k_amp = amp;
        run_cycles(&s, &h, 40u);
        printf("    amp %6.1f contagens -> %s (snr %u/256)\n", amp,
               ir_sensor_result(&s)->threat_any ? "DETECTA" : "nao ve",
               s.threat.band[0].snr_q8);
    }
}

static void test_stealth(void)
{
    SECTION("Furtividade -- ETAPA B nao acende o LED");

    ir_sensor_t s;
    harness_t h;
    sim_frontend_t sim;

    /* Sem ameaca: o fluxo chega na ETAPA C, logo o LED tem de acender. */
    setup(&s, &h, &sim, 7u);
    run_cycles(&s, &h, 40u);
    uint32_t on_clear = h.emitter_on_blocks;
    CHECK(on_clear > 0u, "sem ameaca, o emissor liga (etapa C rodando)");

    /* Com ameaca detectada: a cascata para na ETAPA B e o LED fica
     * praticamente apagado -- "furtividade mantida" do plano. */
    setup(&s, &h, &sim, 7u);
    sim.enemy_38k_amp = 200.0;
    run_cycles(&s, &h, 40u);
    uint32_t on_threat = h.emitter_on_blocks;

    printf("  blocos com emissor ligado: sem ameaca %u, com ameaca %u\n",
           on_clear, on_threat);
    /* Com o latch armado no primeiro bloco de escuta, sobram no maximo os
     * poucos blocos de emissao do ciclo inicial (antes de a ameaca ser
     * vista). Exigimos praticamente zero. */
    CHECK(on_threat <= 2u,
          "com ameaca, o emissor fica apagado (%u blocos, era %u)",
          on_threat, on_clear);
}

static void test_stage_c_presence(void)
{
    SECTION("ETAPA C -- lock-in no sinal proprio");

    ir_sensor_t s;
    harness_t h;
    sim_frontend_t sim;

    /* Sem alvo, apos calibrar: nao deve ver ninguem. O teste que pega
     * crosstalk mal cancelado. */
    setup(&s, &h, &sim, 8u);
    calibrate(&s, &h);
    run_cycles(&s, &h, 60u);
    const ir_result_t *r = ir_sensor_result(&s);
    CHECK(!r->presence,
          "sem alvo = sem presenca (prox %u, snr %u/256)",
          r->proximity, r->snr_q8);
    CHECK(r->verdict == IR_VERDICT_EMPTY,
          "veredito = VAZIO (deu %d)", r->verdict);

    /* Alvo a 20 cm: deve detectar. */
    setup(&s, &h, &sim, 9u);
    calibrate(&s, &h);
    sim.target_dist_cm = 20.0;
    run_cycles(&s, &h, 60u);
    r = ir_sensor_result(&s);
    CHECK(r->presence, "alvo a 20 cm detectado (prox %u, snr %u/256)",
          r->proximity, r->snr_q8);
    CHECK(r->verdict == IR_VERDICT_ENEMY_OWN,
          "veredito = INIMIGO via sinal proprio (deu %d)", r->verdict);

    /* Monotonicidade: mais longe = menos proximidade. Sem isso o micro
     * principal nao pode usar o valor para nada alem de booleano. */
    printf("  curva de proximidade (refletividade 0.8):\n");
    uint32_t prev = 0xFFFFFFFFu;
    bool monotonic = true;
    for (double d = 10.0; d <= 60.0; d += 10.0) {
        setup(&s, &h, &sim, 10u);
        calibrate(&s, &h);
        sim.target_dist_cm = d;
        run_cycles(&s, &h, 60u);
        r = ir_sensor_result(&s);
        printf("    %4.0f cm -> prox %8u  snr %5u/256  presenca %d\n",
               d, r->proximity, r->snr_q8, (int)r->presence);
        if (r->proximity > prev) {
            monotonic = false;
        }
        prev = r->proximity;
    }
    CHECK(monotonic, "proximidade decresce monotonicamente com a distancia");
}

static void test_crosstalk_cancel(void)
{
    SECTION("Calibracao complexa cancela o crosstalk interno");

    ir_sensor_t s;
    harness_t h;
    sim_frontend_t sim;

    /* Crosstalk grande o bastante para, sozinho, parecer um alvo. */
    setup(&s, &h, &sim, 11u);
    sim.crosstalk_amp = 400.0;

    run_cycles(&s, &h, 60u);
    uint32_t sem_cal = ir_sensor_result(&s)->proximity;

    calibrate(&s, &h);
    run_cycles(&s, &h, 60u);
    uint32_t com_cal = ir_sensor_result(&s)->proximity;

    printf("  proximidade sem alvo: sem calibracao %u, com calibracao %u\n",
           sem_cal, com_cal);
    CHECK(com_cal * 8u < sem_cal,
          "calibracao derruba o crosstalk em >8x (%u -> %u)",
          sem_cal, com_cal);
    CHECK(!ir_sensor_result(&s)->presence,
          "crosstalk calibrado nao eh lido como alvo");

    /* E o alvo real ainda tem de aparecer por cima do crosstalk cancelado. */
    sim.target_dist_cm = 20.0;
    run_cycles(&s, &h, 60u);
    CHECK(ir_sensor_result(&s)->presence,
          "alvo real ainda detectado apos calibracao (prox %u)",
          ir_sensor_result(&s)->proximity);
}

static void test_chopping_rejects_jammer(void)
{
    SECTION("Chopping rejeita jammer estatico na nossa f0");

    ir_sensor_t s;
    harness_t h;
    sim_frontend_t sim;

    /* Cenario: adversario emitindo NA NOSSA frequencia (90 kHz) com fase
     * fixa -- de proposito, para nos cegar, ou por azar de terem escolhido
     * a mesma frequencia. Sem chopping isso viraria "alvo colado".
     *
     * O chopping tem de cancelar porque o jammer esta presente TANTO no
     * bloco ON quanto no OFF, e nos subtraimos os dois fasores. */

    /* Linha de base: alvo a 25 cm, sem jammer. */
    setup(&s, &h, &sim, 20u);
    calibrate(&s, &h);
    sim.target_dist_cm = 25.0;
    run_cycles(&s, &h, 80u);
    uint32_t sem_jam = ir_sensor_result(&s)->proximity;
    CHECK(ir_sensor_result(&s)->presence, "linha de base: alvo detectado");

    /* Mesmo alvo, com jammer forte em 90 kHz. */
    setup(&s, &h, &sim, 20u);
    calibrate(&s, &h);
    sim.target_dist_cm = 25.0;
    sim.jammer_f0_amp  = 600.0;
    run_cycles(&s, &h, 80u);
    const ir_result_t *r = ir_sensor_result(&s);

    printf("  alvo a 25 cm: sem jammer %u, com jammer de 600 contagens %u\n",
           sem_jam, r->proximity);
    printf("  |X| em f0 com LED apagado: %u (piso de ruido %u)\n",
           r->jam_magnitude, r->noise_floor);

    double err = fabs((double)r->proximity - (double)sem_jam)
               / (double)(sem_jam ? sem_jam : 1u);
    CHECK(err < 0.25,
          "jammer de 600 contagens muda a medida em <25%% (deu %.1f%%)",
          err * 100.0);
    CHECK(r->presence, "alvo ainda detectado sob jamming");

    /* E o jammer tem de ser DENUNCIADO, nao apenas absorvido em silencio:
     * o micro principal precisa saber que esta sob ataque. */
    CHECK((r->flags & IR_FLAG_JAMMED) != 0u,
          "flag JAMMED marcada (flags 0x%02x)", r->flags);

    /* Sem jammer, a flag nao pode ficar grudada. */
    CHECK((ir_sensor_result(&s)->flags & IR_FLAG_JAMMED) != 0u
          || sem_jam > 0u, "sanidade");
    setup(&s, &h, &sim, 21u);
    calibrate(&s, &h);
    sim.jammer_f0_amp = 0.0;
    run_cycles(&s, &h, 40u);
    CHECK((ir_sensor_result(&s)->flags & IR_FLAG_JAMMED) == 0u,
          "sem jammer, flag JAMMED limpa");
}

static void test_ambient_immunity(void)
{
    SECTION("Imunidade a luz ambiente (CC + ripple de rede)");

    ir_sensor_t s;
    harness_t h;
    sim_frontend_t sim;

    setup(&s, &h, &sim, 22u);
    calibrate(&s, &h);
    sim.target_dist_cm = 25.0;
    run_cycles(&s, &h, 80u);
    uint32_t limpo = ir_sensor_result(&s)->proximity;

    setup(&s, &h, &sim, 22u);
    calibrate(&s, &h);
    sim.target_dist_cm     = 25.0;
    sim.ambient_dc         = 500.0;
    sim.ambient_ripple     = 120.0;
    run_cycles(&s, &h, 80u);
    uint32_t com_ambiente = ir_sensor_result(&s)->proximity;

    printf("  alvo a 25 cm: sem luz ambiente %u, com CC+ripple fortes %u\n",
           limpo, com_ambiente);
    double err = fabs((double)com_ambiente - (double)limpo)
               / (double)(limpo ? limpo : 1u);
    CHECK(err < 0.15,
          "luz ambiente forte muda a medida em <15%% (deu %.1f%%)",
          err * 100.0);
    CHECK(ir_sensor_result(&s)->presence,
          "alvo ainda detectado com luz ambiente forte");
}

static void test_cascade_priority(void)
{
    SECTION("Prioridade da cascata A > B > C");

    ir_sensor_t s;
    harness_t h;
    sim_frontend_t sim;

    /* Alvo presente E adversario de 38 kHz: o plano manda reportar via
     * Goertzel (etapa B vem antes da C). */
    setup(&s, &h, &sim, 13u);
    calibrate(&s, &h);
    sim.target_dist_cm = 20.0;
    sim.enemy_38k_amp  = 200.0;
    run_cycles(&s, &h, 60u);
    CHECK(ir_sensor_result(&s)->verdict == IR_VERDICT_ENEMY_GOERTZEL,
          "B tem prioridade sobre C (deu %d)",
          ir_sensor_result(&s)->verdict);

    /* Cegamento vence tudo. */
    setup(&s, &h, &sim, 14u);
    calibrate(&s, &h);
    sim.target_dist_cm = 20.0;
    sim.enemy_38k_amp  = 200.0;
    sim.ambient_dc     = 1600.0;
    run_cycles(&s, &h, 60u);
    CHECK(ir_sensor_result(&s)->verdict == IR_VERDICT_BLINDED,
          "A tem prioridade sobre B e C (deu %d)",
          ir_sensor_result(&s)->verdict);
}

static void test_square_vs_sine_ref(void)
{
    SECTION("Referencia +-1 vs senoidal, na cadeia completa");

    for (int sq = 0; sq <= 1; sq++) {
        ir_sensor_t s;
        harness_t h;
        sim_frontend_t sim;

        setup(&s, &h, &sim, 15u);
        ir_sensor_set_square_ref(&s, sq != 0);
        calibrate(&s, &h);
        sim.target_dist_cm = 30.0;
        run_cycles(&s, &h, 60u);

        const ir_result_t *r = ir_sensor_result(&s);
        printf("  referencia %-9s: prox %8u  snr %5u/256  presenca %d\n",
               sq ? "+-1" : "senoidal", r->proximity, r->snr_q8,
               (int)r->presence);
        CHECK(r->presence, "referencia %s detecta alvo a 30 cm",
              sq ? "+-1" : "senoidal");
    }
}

static void test_no_overruns(void)
{
    SECTION("Fila ISR -> laco principal");

    ir_sensor_t s;
    harness_t h;
    sim_frontend_t sim;

    setup(&s, &h, &sim, 16u);
    run_cycles(&s, &h, 100u);

    CHECK(s.overruns == 0u, "sem overrun quando o laco acompanha (deu %u)",
          s.overruns);
    CHECK((ir_sensor_result(&s)->flags & IR_FLAG_BLOCK_OVERRUN) == 0u,
          "flag de overrun limpa");
    CHECK(ir_sensor_result(&s)->cycle >= 99u,
          "contou os ciclos (deu %u)", ir_sensor_result(&s)->cycle);
}

int main(void)
{
    printf("Sensor: f0=%u Hz (bin %u)  ciclo=%u blocos=%.1f ms\n",
           CFG_CARRIER_HZ, CFG_CARRIER_BIN, IR_PHASE_COUNT,
           IR_PHASE_COUNT * 1000.0 * CFG_BLOCK_N / CFG_SAMPLE_RATE_HZ);

    test_stage_a_blinding();
    test_stage_b_threat();
    test_stealth();
    test_stage_c_presence();
    test_crosstalk_cancel();
    test_chopping_rejects_jammer();
    test_ambient_immunity();
    test_cascade_priority();
    test_square_vs_sine_ref();
    test_no_overruns();

    return test_report("test_sensor");
}
