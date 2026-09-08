/* ir_sensor.h -- a maquina de estados do sensor. Implementa a cascata do
 * documento de projeto do eletricista ("TUTORIAL INSANO DE COMO IMPLEMENTAR
 * O GOERTZEL + LOCK-IN AMPLIFICATION"):
 *
 *   ETAPA A  media CC do bloco > limiar?     -> CEGAMENTO / INIMIGO COLADO
 *   ETAPA B  Goertzel 38/56 kHz > limiar?    -> INIMIGO (via emissao dele)
 *   ETAPA C  emissor 90 kHz + lock-in +-1    -> INIMIGO (via sinal proprio)
 *            senao                            -> VAZIO
 *
 * A cascata tem saida antecipada, como no plano: se A dispara, nao roda B
 * nem C; se B dispara, nao roda C -- e portanto NAO LIGA O EMISSOR. Isso eh
 * a "furtividade mantida" do plano, e eh de proposito.
 *
 * CICLO DE BLOCOS
 * ---------------
 * O ADC roda continuo em DMA circular de bloco duplo; cada bloco eh 1 ms.
 * O ciclo do sensor tem 4 blocos = 4 ms => 250 decisoes/s.
 *
 *   bloco 0  OFF_LISTEN    emissor OFF, medido: etapas A e B + fundo em 90k
 *   bloco 1  ON_SETTLE     emissor ON,  descartado (front-end assentando)
 *   bloco 2  ON_MEASURE    emissor ON,  medido: etapa C
 *   bloco 3  OFF_SETTLE    emissor OFF, descartado
 *
 * O par (fundo em 90 kHz no bloco 0) e (sinal em 90 kHz no bloco 2) forma
 * um chopper: subtraindo os dois fasores, sai fora qualquer interferencia
 * ESTATICA na nossa propria frequencia -- inclusive outro robo emitindo em
 * 90 kHz de proposito para nos cegar. O chopping cai de graca do fluxo do
 * plano; nao foi preciso inventar ciclo nenhum para ter ele.
 *
 * ONDE CADA COISA RODA
 * --------------------
 * ir_sensor_on_block_isr()  -- chamado do ISR do DMA. Barato: so etiqueta
 *                              o bloco, avanca a fase e chaveia o emissor
 *                              (o chaveamento TEM de ser na fronteira do
 *                              bloco, senao o chopping vaza).
 * ir_sensor_process()       -- chamado do laco principal. Faz todo o DSP.
 *
 * Essa separacao existe porque o DSP de um bloco leva ~2 us no G4 mas o
 * chaveamento do emissor nao pode esperar pelo laco principal. Ver
 * docs/ARQUITETURA.md, "Caminho de tempo real".
 */

#ifndef IR_SENSOR_H
#define IR_SENSOR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../config/sensor_config.h"
#include "../dsp/goertzel.h"
#include "../dsp/lockin.h"
#include "threat_scan.h"
#include "hysteresis.h"
#include "../../port/ports.h"

/* ---------------------------------------------------------------------- */

/* Veredito do sensor -- exatamente as folhas do fluxograma do plano. */
typedef enum {
    IR_VERDICT_EMPTY = 0,      /* IDLE / VAZIO                          */
    IR_VERDICT_ENEMY_OWN,      /* INIMIGO via nosso sinal (etapa C)     */
    IR_VERDICT_ENEMY_GOERTZEL, /* INIMIGO via emissao dele (etapa B)    */
    IR_VERDICT_BLINDED,        /* CEGAMENTO / inimigo colado (etapa A)  */
    IR_VERDICT_CALIBRATING,
} ir_verdict_t;

/* Flags de diagnostico. Vao no relatorio para o micro principal: sem elas,
 * uma falha de front-end vira "nao tem ninguem na minha frente", que eh o
 * pior modo de falha possivel num sumo. */
#define IR_FLAG_SATURATED      (1u << 0)  /* bloco com amostras no trilho */
#define IR_FLAG_DC_HIGH        (1u << 1)  /* media CC fora da faixa       */
#define IR_FLAG_JAMMED         (1u << 2)  /* energia na nossa f0 com LED off */
#define IR_FLAG_BLOCK_OVERRUN  (1u << 3)  /* laco principal perdeu bloco  */
#define IR_FLAG_NO_CROSSTALK   (1u << 4)  /* rodando sem calibracao       */
#define IR_FLAG_AGC_SETTLING   (1u << 5)  /* ganho do PGA acabou de mudar */

typedef struct {
    ir_verdict_t verdict;
    bool     presence;        /* etapa C, apos histerese                 */
    uint32_t proximity;       /* |lock-in| filtrado. Monotonico, nao mm  */
    uint16_t snr_q8;          /* proximity / piso de ruido, Q8           */
    int32_t  phase_q15;       /* fase do lock-in -- validade do sinal    */
    uint32_t noise_floor;
    uint16_t dc_mean;         /* media CC do ultimo bloco de escuta      */
    uint32_t jam_magnitude;   /* |X| em f0 com emissor OFF               */

    bool     threat_any;
    uint16_t threat_freq_khz; /* frequencia da ameaca mais forte         */
    uint8_t  threat_mask;     /* bit i = banda i detectada               */

    uint8_t  pga_gain_index;
    uint16_t flags;
    uint32_t cycle;           /* contador de ciclos de 4 ms              */
} ir_result_t;

/* ---------------------------------------------------------------------- */

typedef enum {
    IR_PHASE_OFF_LISTEN = 0,
    IR_PHASE_ON_SETTLE,
    IR_PHASE_ON_MEASURE,
    IR_PHASE_OFF_SETTLE,
    IR_PHASE_COUNT,
} ir_phase_t;

typedef enum {
    IR_CAL_NONE = 0,
    IR_CAL_CROSSTALK,   /* mede vazamento LED->fotodiodo, sem alvo */
    IR_CAL_DONE,
} ir_cal_state_t;

/* Fila entre o ISR e o laco principal. 2 slots = bloco duplo do DMA. */
#define IR_QUEUE_SLOTS  2u

typedef struct {
    const uint16_t *raw;   /* aponta para a metade do buffer do DMA */
    uint16_t        n;
    uint8_t         phase; /* ir_phase_t vigente DURANTE este bloco */
    bool            valid;
} ir_block_t;

typedef struct {
    ports_t       ports;

    /* --- caminho de ISR ------------------------------------------------ */
    volatile ir_block_t queue[IR_QUEUE_SLOTS];
    volatile uint8_t    q_head;    /* escrito pelo ISR   */
    volatile uint8_t    q_tail;    /* escrito pelo laco  */
    volatile uint32_t   overruns;
    uint8_t             phase;     /* ir_phase_t corrente */

    /* --- DSP ----------------------------------------------------------- */
    int16_t       centered[CFG_BLOCK_N];  /* bloco sem CC */
    gz_t          gz_carrier;
    lockin_sq_t   sq_carrier;             /* variante +-1 do plano */
    lockin_t      lockin;
    threat_scan_t threat;
    hyst_t        presence_hyst;

    fx_cplx_t     phasor_off;             /* fundo em f0 (bloco OFF)  */
    bool          phasor_off_valid;

    uint16_t      dc_est;                 /* media CC do bloco anterior */

    /* --- AGC do PGA ---------------------------------------------------- */
    uint8_t       pga_gain;
    uint8_t       agc_settle;

    /* --- calibracao ---------------------------------------------------- */
    ir_cal_state_t cal_state;
    uint8_t        cal_count;
    int32_t        cal_acc_re;
    int32_t        cal_acc_im;

    /* --- furtividade --------------------------------------------------- */
    /* Latch lido pelo ISR. Enquanto estiver ligado, o emissor NAO acende em
     * fase nenhuma. Tem de ser latch e nao "desliga agora": o ISR religa o
     * emissor a cada fronteira de bloco, entao um desligamento pontual no
     * laco principal so suprimiria UM bloco dos dois de emissao. */
    volatile bool stealth_hold;

    /* --- saida --------------------------------------------------------- */
    ir_result_t   result;
    bool          use_square_ref;   /* true = lock-in +-1 do plano */
} ir_sensor_t;

/* ---------------------------------------------------------------------- */

void ir_sensor_init(ir_sensor_t *s, const ports_t *ports);

/* Escolhe a referencia do lock-in em runtime (util para comparar em
 * bancada). false = Goertzel/senoidal (default), true = +-1. */
void ir_sensor_set_square_ref(ir_sensor_t *s, bool on);

/* Pede uma calibracao de crosstalk. Deve ser feita com o sensor apontado
 * para o VAZIO (sem alvo a menos de ~1 m). */
void ir_sensor_calibrate(ir_sensor_t *s);

/* Chamado do ISR do DMA na fronteira de cada bloco.
 * `raw` aponta para a metade do buffer circular que acabou de encher. */
void ir_sensor_on_block_isr(ir_sensor_t *s, const uint16_t *raw, uint16_t n);

/* Chamado do laco principal. Drena a fila e roda o DSP.
 * Devolve o numero de blocos processados nesta chamada. */
uint8_t ir_sensor_process(ir_sensor_t *s);

const ir_result_t *ir_sensor_result(const ir_sensor_t *s);

#endif /* IR_SENSOR_H */
