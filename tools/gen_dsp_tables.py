#!/usr/bin/env python3
"""Gera as tabelas de coeficientes de DSP usadas pelo core do sensor.

Todas as constantes trigonometricas do firmware saem daqui, em ponto fixo.
Motivo: o core roda sem libm, sem FPU e sem nenhuma chamada de trigonometria
em runtime -- tudo eh determinista e auditavel no binario.

Uso:
    python3 tools/gen_dsp_tables.py --n 300 --fs 300000 \
        --out firmware/core/dsp/fx_tables.h

Regenerar SEMPRE que N (tamanho do bloco) ou Fs mudarem em sensor_config.h.
O header gerado carrega N/Fs e o build falha por _Static_assert se divergirem.
"""

import argparse
import math
import sys

# Q do coeficiente de Goertzel. coeff = 2*cos(w) tem modulo <= 2, logo Q14
# usa no maximo 16 bits (32768). Isso NAO cabe no int32 junto com o estado
# (|s| chega a 2^18), mas o produto coeff*s1 eh feito em int64 de qualquer
# forma -- e em Cortex-M4 SMULL custa 1 ciclo. Entao Q14 eh de graca e da
# 8x mais precisao de fase que Q11. Ver docs/DSP.md, "Headroom".
COEFF_Q = 14
TRIG_Q = 15


def q(value: float, frac_bits: int) -> int:
    return int(round(value * (1 << frac_bits)))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, required=True, help="amostras por bloco")
    ap.add_argument("--fs", type=int, required=True, help="taxa de amostragem [Hz]")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    n, fs = args.n, args.fs
    nbins = n // 2 + 1

    coeff, cos_t, sin_t = [], [], []
    for k in range(nbins):
        w = 2.0 * math.pi * k / n
        coeff.append(q(2.0 * math.cos(w), COEFF_Q))
        cos_t.append(q(math.cos(w), TRIG_Q))
        sin_t.append(q(math.sin(w), TRIG_Q))

    # cos(w) em Q15 chega a 32768, que estoura int16. Guardamos em int32 para
    # nao ter que tratar o caso k=0 como excecao no codigo de leitura.
    def emit(name, values, ctype):
        lines = [f"static const {ctype} {name}[FX_TABLE_BINS] = {{"]
        for i in range(0, len(values), 10):
            lines.append("    " + ", ".join(f"{v:6d}" for v in values[i:i + 10]) + ",")
        lines.append("};")
        return "\n".join(lines)

    body = f"""/* GERADO AUTOMATICAMENTE por tools/gen_dsp_tables.py -- NAO EDITAR A MAO.
 *
 * Recriar com:
 *   python3 tools/gen_dsp_tables.py --n {n} --fs {fs} --out firmware/core/dsp/fx_tables.h
 *
 * Bloco N = {n} amostras, Fs = {fs} Hz
 * Resolucao por bin: Fs/N = {fs / n:.4f} Hz
 * Duracao do bloco:  N/Fs = {1e3 * n / fs:.4f} ms
 */

#ifndef FX_TABLES_H
#define FX_TABLES_H

#include <stdint.h>

#define FX_TABLE_N     {n}
#define FX_TABLE_FS    {fs}
#define FX_TABLE_BINS  {nbins}   /* bins 0 .. N/2 (dobra em Nyquist) */

#define FX_COEFF_Q     {COEFF_Q}
#define FX_TRIG_Q      {TRIG_Q}

/* 2*cos(2*pi*k/N) em Q{COEFF_Q} -- coeficiente da recorrencia de Goertzel */
{emit("fx_goertzel_coeff", coeff, "int32_t")}

/* cos(2*pi*k/N) e sin(2*pi*k/N) em Q{TRIG_Q} -- extracao de Re/Im */
{emit("fx_cos_q15", cos_t, "int32_t")}

{emit("fx_sin_q15", sin_t, "int32_t")}

#endif /* FX_TABLES_H */
"""

    with open(args.out, "w") as fh:
        fh.write(body)

    print(f"{args.out}: N={n} Fs={fs} bins={nbins} df={fs / n:.4f} Hz "
          f"bloco={1e3 * n / fs:.4f} ms")
    return 0


if __name__ == "__main__":
    sys.exit(main())
