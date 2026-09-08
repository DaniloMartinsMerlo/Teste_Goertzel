/* main_host.c -- ferramenta de bancada. Roda o core no PC contra o
 * simulador e imprime CSV, para afinar limiares antes de ter a placa.
 *
 *   ./build/sweep dist          varre distancia do alvo
 *   ./build/sweep threat        varre amplitude do emissor adversario
 *   ./build/sweep noise         varre ruido do front-end
 *   ./build/sweep trace         serie temporal de um cenario de luta
 *
 * Jogue a saida num CSV e plote. Os limiares de sensor_config.h
 * (CFG_PRESENCE_SNR_ON_Q8 etc.) devem ser escolhidos olhando estas curvas
 * com os numeros da SUA optica, nao chutados.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/sensor/ir_sensor.h"
#include "../platform/host/sim_frontend.h"

static const double k_gain_table[CFG_PGA_GAIN_COUNT] = { 2.0, 8.0, 16.0, 32.0 };

typedef struct {
    sim_frontend_t *sim;
    bool emitter_on;
} rig_t;

static void rig_emitter(void *ctx, bool on)
{
    ((rig_t *)ctx)->emitter_on = on;
}

static void rig_pga(void *ctx, uint8_t idx)
{
    rig_t *r = (rig_t *)ctx;
    if (idx >= CFG_PGA_GAIN_COUNT) {
        idx = CFG_PGA_GAIN_COUNT - 1u;
    }
    r->sim->pga_gain = k_gain_table[idx];
}

static uint32_t rig_now(void *ctx)
{
    (void)ctx;
    static uint32_t t;
    t += 1000u;
    return t;
}

static void rig_init(rig_t *r, sim_frontend_t *sim, ir_sensor_t *s,
                     uint64_t seed)
{
    sim_init(sim, seed);
    sim->pga_gain = k_gain_table[CFG_PGA_GAIN_DEFAULT];
    r->sim = sim;
    r->emitter_on = false;

    ports_t p;
    memset(&p, 0, sizeof(p));
    p.emitter.set  = rig_emitter;
    p.emitter.ctx  = r;
    p.pga.set_gain = rig_pga;
    p.pga.ctx      = r;
    p.time.now_us  = rig_now;
    ir_sensor_init(s, &p);
}

static void run(ir_sensor_t *s, rig_t *r, unsigned cycles)
{
    static uint16_t buf[2][CFG_BLOCK_N];
    for (unsigned b = 0u; b < cycles * IR_PHASE_COUNT; b++) {
        r->sim->emitter_on = r->emitter_on;
        uint16_t *slot = buf[b & 1u];
        sim_fill_block(r->sim, slot, CFG_BLOCK_N);
        ir_sensor_on_block_isr(s, slot, CFG_BLOCK_N);
        ir_sensor_process(s);
    }
}

static void calibrate(ir_sensor_t *s, rig_t *r)
{
    double saved = r->sim->target_dist_cm;
    r->sim->target_dist_cm = 0.0;
    ir_sensor_calibrate(s);
    run(s, r, 40u);
    r->sim->target_dist_cm = saved;
}

static const char *verdict_name(ir_verdict_t v)
{
    switch (v) {
    case IR_VERDICT_EMPTY:          return "VAZIO";
    case IR_VERDICT_ENEMY_OWN:      return "INIMIGO_PROPRIO";
    case IR_VERDICT_ENEMY_GOERTZEL: return "INIMIGO_GOERTZEL";
    case IR_VERDICT_BLINDED:        return "CEGADO";
    case IR_VERDICT_CALIBRATING:    return "CALIBRANDO";
    default:                        return "?";
    }
}

/* ---------------------------------------------------------------------- */

static void sweep_dist(void)
{
    printf("dist_cm,refl,proximidade,snr_q8,fase_q15,presenca,veredito\n");

    for (double refl = 0.2; refl <= 0.9; refl += 0.3) {
        for (double d = 5.0; d <= 80.0; d += 2.5) {
            ir_sensor_t s;
            rig_t r;
            sim_frontend_t sim;

            rig_init(&r, &sim, &s, 42u);
            calibrate(&s, &r);
            sim.target_dist_cm = d;
            sim.target_reflect = refl;
            run(&s, &r, 60u);

            const ir_result_t *o = ir_sensor_result(&s);
            printf("%.1f,%.2f,%u,%u,%d,%d,%s\n", d, refl, o->proximity,
                   o->snr_q8, o->phase_q15, (int)o->presence,
                   verdict_name(o->verdict));
        }
    }
}

static void sweep_threat(void)
{
    printf("amp_contagens,mag_38k,snr_38k_q8,detectado,freq_khz,veredito\n");

    for (double amp = 2.0; amp <= 512.0; amp *= 1.4) {
        ir_sensor_t s;
        rig_t r;
        sim_frontend_t sim;

        rig_init(&r, &sim, &s, 7u);
        sim.enemy_38k_amp = amp;
        run(&s, &r, 60u);

        const ir_result_t *o = ir_sensor_result(&s);
        printf("%.1f,%u,%u,%d,%u,%s\n", amp, s.threat.band[0].magnitude,
               s.threat.band[0].snr_q8, (int)s.threat.band[0].detected,
               o->threat_freq_khz, verdict_name(o->verdict));
    }
}

static void sweep_noise(void)
{
    printf("ruido_rms,dist_cm,proximidade,snr_q8,presenca\n");

    for (double nrms = 1.0; nrms <= 128.0; nrms *= 2.0) {
        for (double d = 10.0; d <= 60.0; d += 10.0) {
            ir_sensor_t s;
            rig_t r;
            sim_frontend_t sim;

            rig_init(&r, &sim, &s, 99u);
            sim.noise_rms = nrms;
            calibrate(&s, &r);
            sim.target_dist_cm = d;
            run(&s, &r, 80u);

            const ir_result_t *o = ir_sensor_result(&s);
            printf("%.1f,%.0f,%u,%u,%d\n", nrms, d, o->proximity,
                   o->snr_q8, (int)o->presence);
        }
    }
}

/* Cenario de luta: alvo se aproximando, adversario acende o sensor no
 * meio, e depois tenta nos cegar. Mostra a cascata trocando de veredito. */
static void trace(void)
{
    ir_sensor_t s;
    rig_t r;
    sim_frontend_t sim;

    rig_init(&r, &sim, &s, 2024u);
    calibrate(&s, &r);

    printf("ciclo,t_ms,dist_cm,proximidade,snr_q8,dc,ameaca_khz,flags,veredito\n");

    for (int step = 0; step < 120; step++) {
        double t_ms = step * 20.0;

        /* alvo entrando de 70 cm ate 8 cm */
        sim.target_dist_cm = 70.0 - (double)step * 0.52;
        if (sim.target_dist_cm < 8.0) {
            sim.target_dist_cm = 8.0;
        }

        /* adversario liga o sensor de 38 kHz no meio da aproximacao */
        sim.enemy_38k_amp = (step > 45 && step < 80) ? 150.0 : 0.0;

        /* e tenta cegar no fim */
        sim.ambient_dc = (step > 100) ? 1700.0 : 200.0;

        run(&s, &r, 5u);

        const ir_result_t *o = ir_sensor_result(&s);
        printf("%d,%.0f,%.1f,%u,%u,%u,%u,0x%02x,%s\n", step, t_ms,
               sim.target_dist_cm, o->proximity, o->snr_q8, o->dc_mean,
               o->threat_freq_khz, o->flags, verdict_name(o->verdict));
    }
}

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "dist";

    if (strcmp(mode, "dist") == 0)        sweep_dist();
    else if (strcmp(mode, "threat") == 0) sweep_threat();
    else if (strcmp(mode, "noise") == 0)  sweep_noise();
    else if (strcmp(mode, "trace") == 0)  trace();
    else {
        fprintf(stderr, "uso: %s [dist|threat|noise|trace]\n", argv[0]);
        return 2;
    }
    return 0;
}
