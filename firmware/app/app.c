#include "app.h"

#include <stdio.h>
#include <string.h>

#define APP_REPORT_MAGIC    0x5Au
#define APP_REPORT_VERSION  1u

/* Loga uma linha a cada N ciclos (4 ms cada). 250 ciclos = 1 s. */
#define APP_LOG_EVERY_CYCLES  250u

void app_init(app_t *a, ir_sensor_t *sensor, const ports_t *ports)
{
    a->sensor = sensor;
    a->ports  = *ports;
    a->last_log_cycle = 0u;

    memset(&a->report, 0, sizeof(a->report));
    a->report.magic   = APP_REPORT_MAGIC;
    a->report.version = APP_REPORT_VERSION;
}

static uint16_t clamp_u16(uint32_t v)
{
    return (v > 0xFFFFu) ? 0xFFFFu : (uint16_t)v;
}

static void fill_report(app_t *a, const ir_result_t *r)
{
    sensor_report_t *o = &a->report;

    o->verdict   = (uint8_t)r->verdict;
    o->presence  = r->presence ? 1u : 0u;
    o->flags     = r->flags;

    o->proximity = r->proximity;
    o->snr_q8    = r->snr_q8;

    /* phase_q15 vai de -102944 a +102944 e nao cabe em int16. Dividir por 2
     * mantem 0.006 rad de resolucao, muito mais fina que o erro do
     * estimador (~0.002 rad) -- e economiza 2 bytes no barramento. */
    o->phase_q15 = (int16_t)(r->phase_q15 / 2);

    o->noise_floor   = clamp_u16(r->noise_floor);
    o->dc_mean       = r->dc_mean;
    o->jam_magnitude = clamp_u16(r->jam_magnitude);

    o->threat_mask     = r->threat_mask;
    o->threat_freq_khz = (uint8_t)((r->threat_freq_khz > 255u)
                                   ? 255u : r->threat_freq_khz);
    o->pga_gain_index  = r->pga_gain_index;

    /* seq por ultimo: o leitor compara seq antes e depois de ler o bloco
     * para detectar leitura rasgada. Se seq mudou no meio, le de novo. */
    o->seq++;
}

static void maybe_log(app_t *a, const ir_result_t *r)
{
    if (a->ports.log.write == NULL) {
        return;
    }
    if ((r->cycle - a->last_log_cycle) < APP_LOG_EVERY_CYCLES) {
        return;
    }
    a->last_log_cycle = r->cycle;

    static const char *nomes[] = {
        "VAZIO", "INIMIGO_PROPRIO", "INIMIGO_GOERTZEL", "CEGADO", "CALIBRANDO"
    };
    const char *nome = ((unsigned)r->verdict < 5u)
                     ? nomes[r->verdict] : "?";

    char line[192];
    (void)snprintf(line, sizeof(line),
        "[%lu] %-16s prox=%lu snr=%u/256 dc=%u piso=%lu jam=%lu "
        "ameaca=%ukHz(0x%02x) pga=%u flags=0x%04x\r\n",
        (unsigned long)r->cycle, nome, (unsigned long)r->proximity,
        r->snr_q8, r->dc_mean, (unsigned long)r->noise_floor,
        (unsigned long)r->jam_magnitude, r->threat_freq_khz,
        r->threat_mask, r->pga_gain_index, r->flags);

    a->ports.log.write(a->ports.log.ctx, line);
}

void app_poll(app_t *a)
{
    /* Todo o DSP acontece aqui, fora do ISR. Se este laco atrasar mais que
     * 1 ms, o core conta overrun e levanta IR_FLAG_BLOCK_OVERRUN -- ou seja,
     * a falha aparece no relatorio em vez de virar medida silenciosamente
     * errada. */
    if (ir_sensor_process(a->sensor) == 0u) {
        return;
    }

    const ir_result_t *r = ir_sensor_result(a->sensor);
    fill_report(a, r);
    maybe_log(a, r);
}

const sensor_report_t *app_report(const app_t *a)
{
    return &a->report;
}
