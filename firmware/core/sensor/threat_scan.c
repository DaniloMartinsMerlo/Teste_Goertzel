#include "threat_scan.h"

static const uint16_t k_threat_bins[CFG_THREAT_COUNT] = CFG_THREAT_BINS;

void threat_scan_init(threat_scan_t *t)
{
    t->count     = CFG_THREAT_COUNT;
    t->divider   = (CFG_THREAT_DIVIDER == 0u) ? 1u : CFG_THREAT_DIVIDER;
    t->div_count = 0u;
    t->any_detected = false;
    t->strongest = 0u;

    for (uint8_t i = 0u; i < t->count; i++) {
        gz_init(&t->gz[i], k_threat_bins[i]);

        t->band[i].bin       = k_threat_bins[i];
        t->band[i].freq_khz  = (uint16_t)((k_threat_bins[i] * CFG_BIN_HZ) / 1000u);
        t->band[i].magnitude = 0u;
        t->band[i].snr_q8    = 0u;
        t->band[i].detected  = false;
        t->band[i].hold      = 0u;

        hyst_init(&t->hyst[i], CFG_THREAT_SNR_ON_Q8, CFG_THREAT_SNR_OFF_Q8,
                  3u, 2u);
    }

    gz_init(&t->gz_noise, CFG_NOISE_REF_BIN);

    /* Piso de ruido com constante de tempo longa: ele deve seguir a
     * iluminacao do ambiente (minutos), nao o adversario (milissegundos).
     * shift 6 = tau de 64 blocos de escuta ~= 256 ms. */
    fx_ewma_init(&t->noise_lpf, 6u);
    t->noise_floor = 0u;
}

bool threat_scan_process(threat_scan_t *t, const int16_t *x, uint16_t n)
{
    /* Duty-cycle da varredura. Mesmo com divider > 1 o peak-hold continua
     * decrementando por ciclo, senao a deteccao ficaria retida por
     * divider * hold. */
    if (++t->div_count < t->divider) {
        for (uint8_t i = 0u; i < t->count; i++) {
            if (t->band[i].hold > 0u) {
                t->band[i].hold--;
            }
        }
        return t->any_detected;
    }
    t->div_count = 0u;

    /* --- piso de ruido: bin onde nada eh esperado --------------------- */
    gz_reset(&t->gz_noise);
    gz_push_block(&t->gz_noise, x, n);
    uint32_t noise = gz_magnitude(&t->gz_noise);

    /* Piso 1 evita divisao por zero e SNR infinito quando o front-end
     * esta em silencio absoluto (tipico em bancada, nao em arena). */
    t->noise_floor = (uint32_t)fx_ewma_push(&t->noise_lpf, (int32_t)noise);
    uint32_t floor_pw = t->noise_floor * t->noise_floor;
    if (floor_pw == 0u) {
        floor_pw = 1u;
    }

    /* --- bandas vigiadas --------------------------------------------- */
    bool any = false;
    uint32_t best = 0u;
    uint8_t  best_i = 0u;

    for (uint8_t i = 0u; i < t->count; i++) {
        gz_reset(&t->gz[i]);
        gz_push_block(&t->gz[i], x, n);

        uint64_t pw = gz_power(&t->gz[i]);
        t->band[i].magnitude = fx_isqrt64(pw);

        /* SNR em POTENCIA (fonte incoerente). Clampa o numerador antes de
         * cair no fx_div_q8, que trabalha em 32 bits. */
        uint32_t pw32 = (pw > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)pw;
        t->band[i].snr_q8 = fx_div_q8(pw32, floor_pw);

        bool raw = hyst_push(&t->hyst[i], t->band[i].snr_q8);

        if (raw) {
            t->band[i].hold = CFG_THREAT_HOLD_CYCLES;
        } else if (t->band[i].hold > 0u) {
            t->band[i].hold--;
        }

        t->band[i].detected = raw || (t->band[i].hold > 0u);

        if (t->band[i].detected) {
            any = true;
            if (t->band[i].magnitude > best) {
                best   = t->band[i].magnitude;
                best_i = i;
            }
        }
    }

    t->any_detected = any;
    t->strongest    = best_i;
    return any;
}

uint32_t threat_scan_noise_floor(const threat_scan_t *t)
{
    return t->noise_floor;
}
