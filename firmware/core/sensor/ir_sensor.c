#include "ir_sensor.h"

/* Numero de ciclos de calibracao de crosstalk a promediar. */
#define IR_CAL_CYCLES  16u

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */
/* ---------------------------------------------------------------------- */

static void emitter_set(ir_sensor_t *s, bool on)
{
    if (s->ports.emitter.set != NULL) {
        s->ports.emitter.set(s->ports.emitter.ctx, on);
    }
}

static void pga_set(ir_sensor_t *s, uint8_t gain_index)
{
    if (s->ports.pga.set_gain != NULL) {
        s->ports.pga.set_gain(s->ports.pga.ctx, gain_index);
    }
}

static bool phase_emitter_on(uint8_t phase)
{
    return (phase == IR_PHASE_ON_SETTLE) || (phase == IR_PHASE_ON_MEASURE);
}

/* ---------------------------------------------------------------------- */

void ir_sensor_init(ir_sensor_t *s, const ports_t *ports)
{
    s->ports = *ports;

    for (uint8_t i = 0u; i < IR_QUEUE_SLOTS; i++) {
        s->queue[i].valid = false;
    }
    s->q_head   = 0u;
    s->q_tail   = 0u;
    s->overruns = 0u;
    s->phase    = IR_PHASE_OFF_LISTEN;

    gz_init(&s->gz_carrier, CFG_CARRIER_BIN);
    (void)lockin_sq_init(&s->sq_carrier, CFG_CARRIER_BIN);
    lockin_init(&s->lockin, CFG_LOCKIN_LPF_SHIFT);
    threat_scan_init(&s->threat);

    hyst_init(&s->presence_hyst, CFG_PRESENCE_SNR_ON_Q8,
              CFG_PRESENCE_SNR_OFF_Q8, CFG_PRESENCE_VOTE_N,
              CFG_PRESENCE_VOTE_M);

    s->phasor_off.re = 0;
    s->phasor_off.im = 0;
    s->phasor_off_valid = false;

    s->dc_est = CFG_DC_NOMINAL;

    s->pga_gain   = CFG_PGA_GAIN_DEFAULT;
    s->agc_settle = 0u;

    s->cal_state  = IR_CAL_NONE;
    s->cal_count  = 0u;
    s->cal_acc_re = 0;
    s->cal_acc_im = 0;

    s->use_square_ref = false;
    s->stealth_hold   = false;

    ir_result_t *r = &s->result;
    r->verdict = IR_VERDICT_EMPTY;
    r->presence = false;
    r->proximity = 0u;
    r->snr_q8 = 0u;
    r->phase_q15 = 0;
    r->noise_floor = 0u;
    r->dc_mean = CFG_DC_NOMINAL;
    r->jam_magnitude = 0u;
    r->threat_any = false;
    r->threat_freq_khz = 0u;
    r->threat_mask = 0u;
    r->pga_gain_index = s->pga_gain;
    r->flags = IR_FLAG_NO_CROSSTALK;
    r->cycle = 0u;

    pga_set(s, s->pga_gain);
    emitter_set(s, false);
}

void ir_sensor_set_square_ref(ir_sensor_t *s, bool on)
{
    s->use_square_ref = on;
    lockin_reset_filter(&s->lockin);

    /* O crosstalk calibrado NAO vale entre estimadores: o fasor do Goertzel
     * vem girado de e^(-jw) em relacao ao do lock-in +-1, e as escalas
     * diferem (referencia senoidal vs 3 niveis). Manter a calibracao velha
     * faria a subtracao vetorial apontar para o lado errado. */
    lockin_clear_crosstalk(&s->lockin);
    s->cal_state = IR_CAL_NONE;
    s->result.flags |= IR_FLAG_NO_CROSSTALK;
}

void ir_sensor_calibrate(ir_sensor_t *s)
{
    s->cal_state  = IR_CAL_CROSSTALK;
    s->cal_count  = 0u;
    s->cal_acc_re = 0;
    s->cal_acc_im = 0;
    lockin_clear_crosstalk(&s->lockin);
    lockin_reset_filter(&s->lockin);
    s->result.verdict = IR_VERDICT_CALIBRATING;
    s->result.flags |= IR_FLAG_NO_CROSSTALK;
}

/* ---------------------------------------------------------------------- */
/* Caminho de ISR                                                          */
/* ---------------------------------------------------------------------- */

void ir_sensor_on_block_isr(ir_sensor_t *s, const uint16_t *raw, uint16_t n)
{
    uint8_t head = s->q_head;
    uint8_t next = (uint8_t)((head + 1u) % IR_QUEUE_SLOTS);

    if (next == s->q_tail) {
        /* Laco principal nao acompanhou. Descarta este bloco e conta.
         * Nao trava a fase: perder um bloco eh melhor que travar o
         * chopping fora de sincronia. */
        s->overruns++;
    } else {
        s->queue[head].raw   = raw;
        s->queue[head].n     = n;
        s->queue[head].phase = s->phase;
        s->queue[head].valid = true;
        s->q_head = next;
    }

    /* Avanca a fase e chaveia o emissor JA, na fronteira do bloco.
     * Se isso esperasse pelo laco principal, o bloco ON_MEASURE pegaria
     * parte de bloco com LED apagado e o chopping mediria errado. */
    s->phase = (uint8_t)((s->phase + 1u) % IR_PHASE_COUNT);
    emitter_set(s, phase_emitter_on(s->phase) && !s->stealth_hold);
}

/* ---------------------------------------------------------------------- */
/* DSP -- laco principal                                                   */
/* ---------------------------------------------------------------------- */

/* Remove CC, detecta saturacao e devolve a media do bloco.
 *
 * Usa a media do bloco ANTERIOR para centrar o bloco atual: assim tudo sai
 * em uma passada. Como o CC vem de luz ambiente (varia em ms a s) e o
 * bloco eh de 1 ms, o erro de usar a media anterior eh desprezivel -- e o
 * Goertzel em bin 90 tem ganho CC de apenas ~0.8, entao residuo de CC nao
 * contamina a medida de qualquer forma. */
static uint16_t center_block(ir_sensor_t *s, const uint16_t *raw, uint16_t n,
                             uint16_t *sat_count)
{
    const int32_t dc = (int32_t)s->dc_est;
    const uint16_t hi = (uint16_t)(4095u - CFG_SAT_MARGIN);
    const uint16_t lo = (uint16_t)CFG_SAT_MARGIN;

    uint32_t sum = 0u;
    uint16_t sat = 0u;

    for (uint16_t i = 0u; i < n; i++) {
        uint16_t v = raw[i];
        sum += v;
        if (v >= hi || v <= lo) {
            sat++;
        }
        s->centered[i] = (int16_t)((int32_t)v - dc);
    }

    *sat_count = sat;
    uint16_t mean = (uint16_t)(sum / n);
    s->dc_est = mean;
    return mean;
}

static fx_cplx_t carrier_phasor(ir_sensor_t *s, uint16_t n)
{
    if (s->use_square_ref) {
        lockin_sq_reset(&s->sq_carrier);
        lockin_sq_push_block(&s->sq_carrier, s->centered, n);
        return lockin_sq_phasor(&s->sq_carrier);
    }

    gz_reset(&s->gz_carrier);
    gz_push_block(&s->gz_carrier, s->centered, n);
    return gz_phasor(&s->gz_carrier);
}

/* --- ETAPA A ---------------------------------------------------------- */

static bool stage_a_blinded(ir_sensor_t *s, uint16_t dc_mean, uint16_t sat,
                            uint16_t n)
{
    bool blinded = false;
    s->result.flags &= (uint16_t)~(IR_FLAG_SATURATED | IR_FLAG_DC_HIGH);

    int32_t delta = (int32_t)dc_mean - (int32_t)CFG_DC_NOMINAL;
    if (delta < 0) {
        delta = -delta;
    }
    if (delta > (int32_t)CFG_DC_BLIND_DELTA) {
        s->result.flags |= IR_FLAG_DC_HIGH;
        blinded = true;
    }

    /* Saturacao eh o outro rosto do cegamento: com PGA alto o sinal bate
     * no trilho antes de a MEDIA sair da faixa. Sem esta checagem, um
     * adversario colado apareceria como "vazio" (sinal ceifado perde
     * conteudo em f0). */
    uint32_t sat_q8 = ((uint32_t)sat << 8) / n;
    if (sat_q8 >= CFG_SAT_FRACTION_Q8) {
        s->result.flags |= IR_FLAG_SATURATED;
        blinded = true;
    }

    return blinded;
}

/* --- ETAPA C ---------------------------------------------------------- */

static void stage_c_lockin(ir_sensor_t *s, fx_cplx_t on)
{
    fx_cplx_t off = s->phasor_off;
    if (!s->phasor_off_valid) {
        off.re = 0;
        off.im = 0;
    }

    lockin_push_cycle(&s->lockin, on, off);

    /* Calibracao de crosstalk: promedia o fasor "sem alvo" e grava. Usa
     * `raw` (pos-chopping, pre-crosstalk) porque eh exatamente esse o
     * vetor que queremos anular depois.
     *
     * O guard de stealth_hold nao eh detalhe: sem ele, um ciclo em que a
     * furtividade apagou o emissor entra na media como se fosse crosstalk,
     * e o vetor calibrado sai errado -- o sintoma eh um piso de proximidade
     * que nunca some e o sensor "ve" alvo no vazio. */
    if (s->cal_state == IR_CAL_CROSSTALK) {
        if (s->stealth_hold) {
            return;   /* nao emitimos neste ciclo: nao ha crosstalk a medir */
        }
        s->cal_acc_re += s->lockin.raw.re;
        s->cal_acc_im += s->lockin.raw.im;
        s->cal_count++;

        if (s->cal_count >= IR_CAL_CYCLES) {
            fx_cplx_t xt;
            xt.re = s->cal_acc_re / (int32_t)IR_CAL_CYCLES;
            xt.im = s->cal_acc_im / (int32_t)IR_CAL_CYCLES;
            lockin_set_crosstalk(&s->lockin, xt);
            lockin_reset_filter(&s->lockin);
            s->cal_state = IR_CAL_DONE;
            s->result.flags &= (uint16_t)~IR_FLAG_NO_CROSSTALK;
        }
        return;
    }

    uint32_t floor_mag = threat_scan_noise_floor(&s->threat);
    if (floor_mag == 0u) {
        floor_mag = 1u;
    }

    s->result.proximity = s->lockin.magnitude;
    s->result.phase_q15 = s->lockin.phase_q15;
    s->result.snr_q8    = fx_div_q8(s->lockin.magnitude, floor_mag);

    /* O SNR (nao a magnitude crua) eh o que vai ao limiar. Assim o sensor
     * segue funcionando quando o piso de ruido muda -- sol na arena, motor
     * chaveando ao lado, ganho do PGA diferente. */
    s->result.presence = hyst_push(&s->presence_hyst, s->result.snr_q8);
}

/* --- AGC -------------------------------------------------------------- */

static void agc_update(ir_sensor_t *s)
{
#if CFG_AGC_ENABLE
    if (s->ports.pga.set_gain == NULL) {
        return;
    }

    if (s->agc_settle > 0u) {
        s->agc_settle--;
        s->result.flags |= IR_FLAG_AGC_SETTLING;
        return;
    }
    s->result.flags &= (uint16_t)~IR_FLAG_AGC_SETTLING;

    bool sat = (s->result.flags & (IR_FLAG_SATURATED | IR_FLAG_DC_HIGH)) != 0u;

    if (sat && s->pga_gain > 0u) {
        s->pga_gain--;
    } else if (!sat && s->result.snr_q8 < CFG_AGC_SNR_LOW_Q8
               && s->pga_gain < (CFG_PGA_GAIN_COUNT - 1u)) {
        s->pga_gain++;
    } else {
        return;
    }

    /* Trocar de ganho muda a escala de I/Q E invalida o crosstalk medido
     * no ganho anterior. Zerar o filtro evita um transiente que o decisor
     * leria como alvo aparecendo/desaparecendo. */
    pga_set(s, s->pga_gain);
    lockin_reset_filter(&s->lockin);
    s->agc_settle = CFG_AGC_SETTLE_CYCLES;
    s->result.pga_gain_index = s->pga_gain;
    s->result.flags |= IR_FLAG_AGC_SETTLING;
#else
    (void)s;
#endif
}

/* --- veredito --------------------------------------------------------- */

static void decide(ir_sensor_t *s, bool blinded)
{
    ir_result_t *r = &s->result;

    if (s->cal_state == IR_CAL_CROSSTALK) {
        r->verdict = IR_VERDICT_CALIBRATING;
        return;
    }

    /* A cascata do plano, na ordem do plano. */
    if (blinded) {
        r->verdict = IR_VERDICT_BLINDED;
    } else if (r->threat_any) {
        r->verdict = IR_VERDICT_ENEMY_GOERTZEL;
    } else if (r->presence) {
        r->verdict = IR_VERDICT_ENEMY_OWN;
    } else {
        r->verdict = IR_VERDICT_EMPTY;
    }
}

/* ---------------------------------------------------------------------- */

static void process_one(ir_sensor_t *s, const ir_block_t *blk)
{
    uint16_t sat = 0u;
    uint16_t dc  = center_block(s, blk->raw, blk->n, &sat);

    switch ((ir_phase_t)blk->phase) {

    case IR_PHASE_OFF_LISTEN: {
        s->result.dc_mean = dc;

        bool blinded = stage_a_blinded(s, dc, sat, blk->n);

        /* ETAPA B roda mesmo com cegamento: saber QUE frequencia esta nos
         * cegando eh informacao tatica (diz qual sensor o adversario usa). */
        bool threat = threat_scan_process(&s->threat, s->centered, blk->n);

        s->result.threat_any  = threat;
        s->result.noise_floor = threat_scan_noise_floor(&s->threat);
        s->result.threat_mask = 0u;
        for (uint8_t i = 0u; i < s->threat.count; i++) {
            if (s->threat.band[i].detected) {
                s->result.threat_mask |= (uint8_t)(1u << i);
            }
        }
        s->result.threat_freq_khz = threat
            ? s->threat.band[s->threat.strongest].freq_khz
            : 0u;

        /* Fundo na NOSSA frequencia, com o LED apagado. Serve para duas
         * coisas: eh o termo OFF do chopper, e eh o detector de jamming
         * deliberado em 90 kHz. */
        fx_cplx_t off = carrier_phasor(s, blk->n);
        s->phasor_off = off;
        s->phasor_off_valid = true;

        s->result.jam_magnitude = fx_cplx_abs(off);
        uint32_t nf = s->result.noise_floor ? s->result.noise_floor : 1u;
        if (fx_div_q8(s->result.jam_magnitude, nf) >= CFG_THREAT_SNR_ON_Q8) {
            s->result.flags |= IR_FLAG_JAMMED;
        } else {
            s->result.flags &= (uint16_t)~IR_FLAG_JAMMED;
        }

        /* Saida antecipada da cascata: se A ou B pegou alguem, nao ha
         * motivo para acender o LED e entregar nossa posicao ao adversario.
         * Arma o latch; o ISR passa a suprimir as fases de emissao.
         *
         * Custo consciente: sem emissao nao ha ETAPA C, logo perdemos a
         * medida de distancia enquanto a ameaca durar. O plano pede
         * "furtividade mantida" explicitamente, entao esse eh o default;
         * quem preferir manter o alcance troca CFG_STEALTH_ON_THREAT. */
#if CFG_STEALTH_ON_THREAT
        bool hold = blinded || threat;
#else
        bool hold = blinded;
#endif
        /* Calibracao precisa do emissor ligado. Se a furtividade a
         * atropelasse, a calibracao nunca terminaria (ou terminaria com um
         * vetor medido sem emissao, que eh pior). */
        if (s->cal_state == IR_CAL_CROSSTALK) {
            hold = false;
        }
        s->stealth_hold = hold;
        if (hold) {
            emitter_set(s, false);
            /* Nao estamos medindo a etapa C: reportar a ultima presenca
             * conhecida seria mentir. Zera a decisao e a janela de votacao. */
            s->result.presence = false;
            hyst_reset(&s->presence_hyst);
            lockin_reset_filter(&s->lockin);
            s->result.proximity = 0u;
            s->result.snr_q8 = 0u;
        }

        decide(s, blinded);
        break;
    }

    case IR_PHASE_ON_MEASURE: {
        fx_cplx_t on = carrier_phasor(s, blk->n);
        stage_c_lockin(s, on);
        agc_update(s);
        decide(s, (s->result.flags
                   & (IR_FLAG_SATURATED | IR_FLAG_DC_HIGH)) != 0u);

        s->result.cycle++;
        break;
    }

    case IR_PHASE_ON_SETTLE:
    case IR_PHASE_OFF_SETTLE:
    default:
        /* Descartado de proposito: front-end assentando apos o emissor
         * chavear. Ainda assim passou por center_block(), o que mantem
         * dc_est atualizado a cada 1 ms. */
        break;
    }
}

uint8_t ir_sensor_process(ir_sensor_t *s)
{
    uint8_t done = 0u;

    while (s->q_tail != s->q_head) {
        ir_block_t blk;
        uint8_t tail = s->q_tail;

        blk.raw   = s->queue[tail].raw;
        blk.n     = s->queue[tail].n;
        blk.phase = s->queue[tail].phase;
        blk.valid = s->queue[tail].valid;

        s->queue[tail].valid = false;
        s->q_tail = (uint8_t)((tail + 1u) % IR_QUEUE_SLOTS);

        if (blk.valid && blk.raw != NULL && blk.n == CFG_BLOCK_N) {
            process_one(s, &blk);
            done++;
        }
    }

    if (s->overruns != 0u) {
        s->result.flags |= IR_FLAG_BLOCK_OVERRUN;
    }

    return done;
}

const ir_result_t *ir_sensor_result(const ir_sensor_t *s)
{
    return &s->result;
}
