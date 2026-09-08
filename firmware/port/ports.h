/* ports.h -- os contratos que o core do sensor conhece do mundo externo.
 *
 * Regra da arquitetura: o core (firmware/core/) inclui APENAS isto.
 * Nunca stm32*_hal.h, nunca board_config.h, nunca nada de plataforma.
 * Quem implementa estas funcoes sao os adapters em
 * firmware/platform/<plataforma>/. Por isso o core roda identico no PC
 * (firmware/platform/host) e no G4.
 *
 * Tudo aqui eh uma struct de ponteiros de funcao + um ctx opaco: sem
 * variavel global, sem singleton, e o teste pode injetar um duble.
 */

#ifndef PORTS_H
#define PORTS_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------- */

/* Emissor IR: liga/desliga a portadora (o PWM em si fica configurado pelo
 * adapter; o core so decide QUANDO ligar). Precisa ser barato: eh chamado
 * de dentro do ISR do DMA, na fronteira do bloco. */
typedef struct {
    void (*set)(void *ctx, bool on);
    void  *ctx;
} port_emitter_t;

/* PGA (OPAMP interno do G4 em modo high-speed). gain_index eh um indice
 * em 0..CFG_PGA_GAIN_COUNT-1, nao um valor de ganho: o mapa
 * indice -> OPAMP_PGA_GAIN_x mora no adapter.
 *
 * set_gain pode ser NULL quando nao houver PGA -- o core desliga o AGC. */
typedef struct {
    void (*set_gain)(void *ctx, uint8_t gain_index);
    void  *ctx;
} port_pga_t;

/* Tempo monotonico em microssegundos. Usado para timestamp e watchdog de
 * bloco; nao entra em nenhuma conta de DSP (a base de tempo do DSP eh a
 * contagem de blocos, que eh exata). */
typedef struct {
    uint32_t (*now_us)(void *ctx);
    void     *ctx;
} port_time_t;

/* Log de diagnostico. Pode ser NULL em producao. Nunca chamado do ISR. */
typedef struct {
    void (*write)(void *ctx, const char *line);
    void  *ctx;
} port_log_t;

/* Conjunto completo entregue ao core na inicializacao. */
typedef struct {
    port_emitter_t emitter;
    port_pga_t     pga;
    port_time_t    time;
    port_log_t     log;
} ports_t;

#endif /* PORTS_H */
