#include "hysteresis.h"

void hyst_init(hyst_t *h, uint16_t thr_on, uint16_t thr_off,
               uint8_t vote_n, uint8_t vote_m)
{
    if (vote_n == 0u) {
        vote_n = 1u;
    }
    if (vote_n > HYST_MAX_WINDOW) {
        vote_n = HYST_MAX_WINDOW;
    }
    if (vote_m == 0u) {
        vote_m = 1u;
    }
    if (vote_m > vote_n) {
        vote_m = vote_n;
    }
    if (thr_off > thr_on) {
        thr_off = thr_on;
    }

    h->thr_on  = thr_on;
    h->thr_off = thr_off;
    h->vote_n  = vote_n;
    h->vote_m  = vote_m;
    hyst_reset(h);
}

void hyst_reset(hyst_t *h)
{
    h->window = 0u;
    h->filled = 0u;
    h->state  = false;
}

static uint8_t popcount16(uint16_t v)
{
    uint8_t c = 0u;
    while (v != 0u) {
        v &= (uint16_t)(v - 1u);
        c++;
    }
    return c;
}

bool hyst_push(hyst_t *h, uint16_t value)
{
    /* Decisao bruta com histerese: o limiar depende do estado atual. */
    bool raw = h->state ? (value >= h->thr_off)
                        : (value >= h->thr_on);

    h->window = (uint16_t)(((h->window << 1) | (raw ? 1u : 0u))
                           & ((1u << h->vote_n) - 1u));
    if (h->filled < h->vote_n) {
        h->filled++;
    }

    uint8_t votes = popcount16(h->window);

    /* Janela ainda enchendo. Aqui NAO se pode seguir a decisao bruta: um
     * unico bloco espurio na largada ligaria o detector, e a jusante isso
     * arma peak-hold e latch de furtividade -- o sensor passa a luta inteira
     * sem emitir. Exige unanimidade das amostras que ja existem: responde
     * rapido a um sinal de verdade e ignora glitch isolado. */
    if (h->filled < h->vote_n) {
        h->state = (votes == h->filled) && (h->filled > 0u);
        return h->state;
    }

    if (!h->state && votes >= h->vote_m) {
        h->state = true;
    } else if (h->state && votes <= (uint8_t)(h->vote_n - h->vote_m)) {
        h->state = false;
    }

    return h->state;
}

bool hyst_state(const hyst_t *h)
{
    return h->state;
}
