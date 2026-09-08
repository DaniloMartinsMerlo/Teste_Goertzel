# Requisitos de hardware do front-end

Três itens aqui são **bloqueantes**: sem eles o firmware está correto e o
sensor não funciona. Estão marcados como ⚠️.

## ⚠️ 1. Fototransistor não serve. Precisa ser fotodiodo PIN

Um fototransistor comum (TCRT5000, os de par emissor/receptor de prateleira)
tem tempo de subida de 10–15 µs, o que põe o −3 dB em torno de 20–30 kHz.
Em **90 kHz ele está ~15 dB ou mais abaixo** — o sinal simplesmente não
chega.

Use **fotodiodo PIN**: BPW34, SFH203P, VBPW34S ou equivalente.

E **polarize reverso**. Sem polarização o BPW34 tem ~72 pF de capacitância;
com alguns volts de reverso cai para 10–20 pF, o que é o que permite o TIA
alcançar a banda necessária.

## ⚠️ 2. Filtro anti-aliasing é obrigatório

Fs = 300 kSPS ⇒ tudo acima de 150 kHz dobra para dentro da banda. Dois
motivos para o filtro:

- **Harmônicas do PWM.** A 9ª (810 kHz) e a 11ª (990 kHz) dobram exatamente
  em cima do bin da portadora (ver `docs/DSP.md`, seção 3). São −19 dBc na
  origem; o filtro é o que as torna irrelevantes.
- **Ruído de banda larga.** Todo ruído acima de 150 kHz dobra para dentro e
  levanta o piso, comendo diretamente o alcance.

**Especificação:** passa-baixa de **2 polos em ~130 kHz** (Sallen-Key). Isso
dá ~−16 dB em 270 kHz e ~−36 dB em 810 kHz.

O OPAMP interno do G4 em modo high-speed tem ~13 MHz de GBW e **não** rola
sozinho. O filtro tem de ser projetado, não esperado.

## ⚠️ 3. O PGA precisa de modo high-speed

O plano já diz isso, e a conta confirma. Banda de malha fechada = GBW/ganho:

| modo | GBW | ganho 16 | ganho 32 | ganho 64 |
|---|---|---|---|---|
| normal | ~1.6 MHz | 100 kHz ❌ | 50 kHz ❌ | 25 kHz ❌ |
| **high-speed** | ~13 MHz | **810 kHz** ✅ | **400 kHz** ✅ | 200 kHz ⚠️ |

Com portadora em 90 kHz é preciso banda ≥ ~3·f₀ = 270 kHz para resposta
plana. Portanto: **high-speed obrigatório, ganho máximo 32**. Ganho 64 fica
no limite e é melhor evitar.

Em modo normal, nem o ganho 16 fecha — o sensor mediria uma fração do sinal
e a culpa pareceria ser do algoritmo.

## Topologia recomendada

O G4 tem 3 OPAMPs (algumas variantes 6). Vale usar dois:

```
fotodiodo PIN          OPAMP1              filtro            OPAMP2           ADC
(polarizado reverso)   standalone          Sallen-Key        modo PGA
      |                Rf/Cf externos      2 polos           interno,
      +--------------> TIA -------------->  ~130 kHz -----> high-speed ----> canal
                       ~100 kOhm                            ganho 2..32       interno
```

Por que dois estágios: o **modo PGA usa resistores internos**, então não dá
para fazer TIA com ele (TIA precisa de Rf/Cf externos). E o **modo standalone
não tem PGA programável**. Um OPAMP para cada função resolve, e o AGC do
firmware ganha um ganho de verdade para mexer.

Banda do TIA: `f_-3dB ≈ sqrt(GBW / (2π·Rf·Cin))`. Com GBW = 13 MHz,
Rf = 100 kΩ e Cin = 25 pF ⇒ ~900 kHz. Folga confortável.

## Polarização e faixa do ADC

O sinal é AC em torno de um nível DC. O firmware espera esse nível em
**~VREF/2 = 2048 contagens** (`CFG_DC_NOMINAL`). O DAC interno do G4 é uma
forma limpa de gerar essa referência.

`CFG_DC_BLIND_DELTA = 1200` significa: média fora de 2048 ± 1200 é
classificada como cegamento. Ajuste depois de medir a média real com o LED
apagado, na iluminação da arena.

## Caminho ADC interno

O G4 permite rotear a saída do OPAMP **internamente** ao ADC (p.ex.
OPAMP2_VOUT → um canal interno do ADC2; confira a tabela de conexões
internas do RM da sua variante). Prefira esse caminho: não gasta pino e não
capta ruído de placa.

Não use **oversampling de hardware**. Ele faz média de amostras
*consecutivas*, o que aqui mistura fases diferentes da portadora
(Fs/f₀ = 10/3) e destrói exatamente a informação de fase em que o lock-in se
sustenta. A média que interessa é a coerente, e ela é feita no core.

## Crosstalk óptico interno

O vazamento LED → fotodiodo dentro do encapsulamento é real e costuma ser
grande. **Bloqueie mecanicamente**: barreira opaca entre emissor e receptor,
tubos/colimadores separados.

O firmware cancela o resíduo por subtração vetorial (é coerente, tem fase
fixa — por isso sai no plano complexo, não em magnitude). Nos testes o
cancelamento é de **~400×**. Mas cancelar não recupera faixa dinâmica
gasta: se o crosstalk saturar o PGA, não há software que resolva.

## Corrente do LED

Duty de 10% permite pico alto com média baixa. Um LED de 100 mA contínuo
aguenta ~1 A de pico em 10%, e alcance vai com a corrente de pico. Vale
dimensionar o driver (MOSFET + resistor de emissor) para o pico, não para a
média — e conferir o limite de pulso no datasheet do LED.

## Checklist de bancada

1. **Sem AAF nem fotodiodo rápido, não comece.** São os itens ⚠️.
2. Osciloscópio na saída do PGA, LED ligado, alvo branco a 10 cm: deve haver
   senoide visível de 90 kHz. Se não houver, o problema é óptico/analógico,
   não firmware.
3. Meça a média com LED apagado na luz da arena → calibre
   `CFG_DC_BLIND_DELTA`.
4. Rode `./build/sweep dist` e compare a curva simulada com a medida real
   para calibrar `CFG_PRESENCE_SNR_ON_Q8`.
5. Calibração de crosstalk **apontando para o vazio** (nada a menos de ~1 m).
   Calibrar com alvo na frente grava o alvo como vazamento e cega o sensor
   justamente para ele.
