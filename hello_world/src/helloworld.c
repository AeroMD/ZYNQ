/*******************************************************************************
 * helloworld.c — Zynq-7000 Bare-Metal Active Noise Cancellation System
 *
 * Board  : Digilent Cora Z7-07S  (xc7z007sclg400-1)
 * Tools  : Vitis 2025.2 / Vivado 2025.2
 * Target : Cortex-A9 bare-metal, no OS
 *
 * Signal flow:
 *   Piezo ref   (VP/VN)      ─┐
 *   Piezo error (VAUXP0/VN0) ─┴─► xadc_wiz_0 (16-bit interleaved stream)
 *                                  ──► lms_filter_0  (AXI Stream, ap_ctrl_none)
 *                                       ├─ m_axis_e ──► axi_dma_0/S2MM ──► e_buf (DDR)
 *                                       └─ m_axis_y ──► axi_dma_1/S2MM ──► y_buf (DDR)
 *
 *   ARM reads both buffers each batch:
 *     - Prints e(n) over UART for convergence monitoring
 *     - Writes each y(n) sample as a TTC PWM duty cycle → Circ3 → anti-noise
 *       transducer (10 kHz carrier, Circ3 low-pass removes it)
 ******************************************************************************/

#include "platform.h"
#include "xil_printf.h"
#include "xaxidma.h"
#include "xttcps.h"
#include "xparameters.h"
#include "xstatus.h"
#include "xil_cache.h"
#include "sleep.h"

/* ===========================================================================
 * Constants
 * =========================================================================*/

/* Number of LMS sample pairs captured per DMA transfer                      */
#define SAMPLES_PER_TRANSFER    128U

/* Each stream outputs one float32 per sample — transfer = 128 × 4 bytes    */
#define DMA_TRANSFER_SIZE       (SAMPLES_PER_TRANSFER * sizeof(u32))   /* 512 B */

/* Maximum polling iterations before declaring a DMA timeout                 */
#define DMA_TIMEOUT_COUNT       1000000U

/* XADC clock divisor: ADC_CLK = DCLK / 4  (hardware default from Vivado).
 * DCLK = 104 MHz (clk_wiz_0 output), divisor = 4 → ADC_CLK ≈ 26 MHz
 * → ~1 MSps total across both channels (500 kSps per channel).             */
#define XADC_CLK_DIVISOR        4U

/* Delay between printed batches (microseconds)                              */
#define LOOP_DELAY_US           500000U

/* TTC PWM carrier frequency for anti-noise output (Hz)                     */
#define TTC_CARRIER_FREQ_HZ     10000U

/* ===========================================================================
 * XPAR helpers — SDT uses base addresses, classic flow uses device IDs
 * =========================================================================*/
#ifdef SDT
#  define DMA0_CFG_ARG   XPAR_XAXIDMA_0_BASEADDR
#  define DMA1_CFG_ARG   XPAR_XAXIDMA_1_BASEADDR
#  define TTC0_CFG_ARG   XPAR_XTTCPS_0_BASEADDR
#else
#  define DMA0_CFG_ARG   ((u32)0U)
#  define DMA1_CFG_ARG   ((u32)1U)
#  define TTC0_CFG_ARG   ((u32)0U)
#endif

/* ===========================================================================
 * Global: TTC interval stored at init, used each loop iteration
 * =========================================================================*/
static XInterval g_ttc_period = 0U;

/* ===========================================================================
 * print_float
 *   Prints a float as "label: ±int.frac" using integer arithmetic only.
 *   xil_printf does not support %%f on bare-metal Zynq.
 * =========================================================================*/
static void print_float(const char *label, float v)
{
    int   sign   = (v < 0.0f) ? 1 : 0;
    float absv   = sign ? -v : v;
    int   i_part = (int)absv;
    int   f_part = (int)((absv - (float)i_part) * 1000.0f);

    if (sign) {
        xil_printf("%s: -%d.%03d", label, i_part, f_part);
    } else {
        xil_printf("%s:  %d.%03d", label, i_part, f_part);
    }
}

/* ===========================================================================
 * init_xadc
 *   Confirms XADC Wizard configuration (set at synthesis by Vivado).
 *   The Wizard runs in continuous Channel Sequencer mode with VP/VN and
 *   VAUXP0 enabled, interleaving them into the AXI-Stream output:
 *     [VP/VN, VAUXP0, VP/VN, VAUXP0, ...]  at ~1 MSps total.
 *   No software register writes are needed: XSysMon_CfgInitialize resets
 *   the XADC DRP registers, discarding the Vivado configuration; skipping
 *   it lets the hardware-programmed config remain intact.
 * =========================================================================*/
static int init_xadc(void)
{
    /* The XADC Wizard is fully configured at synthesis time by Vivado:
     *   C_CONFIGURATION_R1 = 8353  → bits[15:12]=2  continuous sequencer
     *   C_SEQUENCE_R0      = 2048  → bit 11 = VP/VN  enabled
     *   C_SEQUENCE_R1      = 1     → bit  0 = VAUXP0 enabled
     *   C_CONFIGURATION_R2 = 1024  → divisor = 4 → DCLK/4 ≈ 26 MHz ADC_CLK
     *   C_SAMPLING_RATE    = 1e6   → 1 MSps total, 500 kSps per channel
     *
     * XSysMon_CfgInitialize calls XSysMon_Reset(), which overwrites all of
     * the above DRP registers with power-on defaults (single-channel
     * temperature mode, divisor = 10).  We avoid that reset entirely:
     * the XADC Wizard begins streaming on power-up with the Vivado config
     * and we do not need the software driver for anything in the main loop
     * (all ADC data arrives via DMA, not via XSysMon register reads).       */
    xil_printf("XADC: VP/VN (x_in) + VAUXP0 (d_in), "
               "continuous sequencer, DCLK/%u (~1 MSps, Vivado config)\r\n",
               (unsigned int)XADC_CLK_DIVISOR);
    return XST_SUCCESS;
}

/* ===========================================================================
 * init_dma
 *   Configures one AXI DMA instance in S2MM-only polling mode.
 *   Called twice: once for DMA0 (e stream), once for DMA1 (y stream).
 *   cfg_arg is the base address (SDT) or device ID (classic flow).
 * =========================================================================*/
static int init_dma(XAxiDma *inst, u32 cfg_arg, const char *label)
{
    XAxiDma_Config *cfg;
    int status;
    u32 timeout;

    cfg = XAxiDma_LookupConfig(cfg_arg);
    if (cfg == NULL) {
        xil_printf("ERROR: %s LookupConfig failed\r\n", label);
        return XST_FAILURE;
    }

    status = XAxiDma_CfgInitialize(inst, cfg);
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: %s CfgInitialize failed (status=%d)\r\n",
                   label, status);
        return XST_FAILURE;
    }

    XAxiDma_Reset(inst);
    timeout = DMA_TIMEOUT_COUNT;
    while (!XAxiDma_ResetIsDone(inst)) {
        if (--timeout == 0U) {
            xil_printf("ERROR: %s reset timed out\r\n", label);
            return XST_FAILURE;
        }
    }

    /* Disable all S2MM interrupts — polling mode                            */
    XAxiDma_IntrDisable(inst, XAXIDMA_IRQ_ALL_MASK, XAXIDMA_DEVICE_TO_DMA);

    xil_printf("%s: S2MM polling mode, interrupts disabled\r\n", label);
    return XST_SUCCESS;
}

/* ===========================================================================
 * init_ttc
 *   Configures TTC0 Timer-0 as a 10 kHz PWM carrier for the y(n) anti-noise
 *   output path (Circ3 low-pass filter removes the 10 kHz carrier leaving
 *   only the reconstructed y(n) waveform to drive the feedback transducer).
 *
 *   Match register 0 sets duty cycle:
 *       duty = match / g_ttc_period  (0 % … 100 %)
 *   Initialised to 50 % (silence).  Main loop updates it per y(n) sample.
 *
 *   Physical output pin: enable TTC0 EMIO in the PS7 IP within Vivado, then
 *   constrain the EMIO waveform port to the PMOD pin feeding Circ3.
 * =========================================================================*/
static int init_ttc(XTtcPs *inst)
{
    XTtcPs_Config *cfg;
    int status;
    u8  prescaler;

    cfg = XTtcPs_LookupConfig(TTC0_CFG_ARG);
    if (cfg == NULL) {
        xil_printf("ERROR: TTC LookupConfig failed\r\n");
        return XST_FAILURE;
    }

    status = XTtcPs_CfgInitialize(inst, cfg, cfg->BaseAddress);
    if (status != XST_SUCCESS) {
        xil_printf("ERROR: TTC CfgInitialize failed (status=%d)\r\n", status);
        return XST_FAILURE;
    }

    /* Interval mode + waveform output (active-high polarity)                */
    XTtcPs_SetOptions(inst, XTTCPS_OPTION_INTERVAL_MODE |
                            XTTCPS_OPTION_WAVE_POLARITY);

    /* BSP selects the best prescaler to achieve TTC_CARRIER_FREQ_HZ        */
    XTtcPs_CalcIntervalFromFreq(inst, TTC_CARRIER_FREQ_HZ,
                                &g_ttc_period, &prescaler);
    XTtcPs_SetPrescaler(inst, prescaler);
    XTtcPs_SetInterval(inst, g_ttc_period);

    /* 50 % duty cycle = silence until LMS has valid data                    */
    XTtcPs_SetMatchValue(inst, 0, (XInterval)(g_ttc_period >> 1U));

    XTtcPs_Start(inst);

    xil_printf("TTC0: %u Hz PWM, period=%u counts, prescaler=%u\r\n",
               (unsigned int)TTC_CARRIER_FREQ_HZ,
               (unsigned int)g_ttc_period,
               (unsigned int)prescaler);
    return XST_SUCCESS;
}

/* ===========================================================================
 * main
 * =========================================================================*/
int main(void)
{
    init_platform();

    xil_printf("\r\n========================================\r\n");
    xil_printf("  Zynq-7000 ANC System  (Cora Z7-07S)  \r\n");
    xil_printf("  Dual DMA | TTC PWM anti-noise output  \r\n");
    xil_printf("========================================\r\n\r\n");

    static XAxiDma dma0;    /* e(n) — LMS m_axis_e → DMA0 */
    static XAxiDma dma1;    /* y(n) — LMS m_axis_y → DMA1 */
    static XTtcPs  ttc;

    /* DDR receive buffers — 32-byte aligned for Cortex-A9 cache-line ops.
     *   e_buf[i] : e(n) sample i  (float32, stored as u32 bit pattern)
     *   y_buf[i] : y(n) sample i  (float32, stored as u32 bit pattern)     */
    static u32 e_buf[SAMPLES_PER_TRANSFER] __attribute__((aligned(32)));
    static u32 y_buf[SAMPLES_PER_TRANSFER] __attribute__((aligned(32)));

    /* --- Peripheral initialisation ---------------------------------------- */
    if (init_xadc() != XST_SUCCESS) {
        xil_printf("FATAL: XADC init failed\r\n");
        cleanup_platform();
        return XST_FAILURE;
    }

    if (init_dma(&dma0, DMA0_CFG_ARG, "DMA0(e)") != XST_SUCCESS) {
        xil_printf("FATAL: DMA0 init failed\r\n");
        cleanup_platform();
        return XST_FAILURE;
    }

    if (init_dma(&dma1, DMA1_CFG_ARG, "DMA1(y)") != XST_SUCCESS) {
        xil_printf("FATAL: DMA1 init failed\r\n");
        cleanup_platform();
        return XST_FAILURE;
    }

    if (init_ttc(&ttc) != XST_SUCCESS) {
        xil_printf("FATAL: TTC init failed\r\n");
        cleanup_platform();
        return XST_FAILURE;
    }

    xil_printf("\r\nAll peripherals ready.  "
               "LMS running in PL.  Starting ANC loop...\r\n\r\n");

    u32 batch = 0U;

    /* --- Main loop -------------------------------------------------------- */
    while (1) {

        /* Step 1 — Invalidate D-cache before arming so no dirty CPU lines
         *          can overwrite DDR after the DMA starts writing.           */
        Xil_DCacheInvalidateRange((UINTPTR)e_buf, DMA_TRANSFER_SIZE);
        Xil_DCacheInvalidateRange((UINTPTR)y_buf, DMA_TRANSFER_SIZE);

        /* Step 2 — Arm both S2MM channels simultaneously.
         *          DMA0 fills e_buf; DMA1 fills y_buf.                      */
        int s0 = XAxiDma_SimpleTransfer(&dma0, (UINTPTR)e_buf,
                                        DMA_TRANSFER_SIZE,
                                        XAXIDMA_DEVICE_TO_DMA);
        int s1 = XAxiDma_SimpleTransfer(&dma1, (UINTPTR)y_buf,
                                        DMA_TRANSFER_SIZE,
                                        XAXIDMA_DEVICE_TO_DMA);
        if (s0 != XST_SUCCESS || s1 != XST_SUCCESS) {
            xil_printf("ERROR: DMA kick failed (s0=%d s1=%d)\r\n", s0, s1);
            cleanup_platform();
            return XST_FAILURE;
        }

        /* Step 3 — Poll until both S2MM channels finish.
         *          Independent timeout counters so a stall is identified.   */
        u32 t0 = DMA_TIMEOUT_COUNT;
        u32 t1 = DMA_TIMEOUT_COUNT;
        while (XAxiDma_Busy(&dma0, XAXIDMA_DEVICE_TO_DMA) ||
               XAxiDma_Busy(&dma1, XAXIDMA_DEVICE_TO_DMA)) {
            if (XAxiDma_Busy(&dma0, XAXIDMA_DEVICE_TO_DMA) && --t0 == 0U) {
                xil_printf("ERROR: DMA0 S2MM timed out\r\n");
                cleanup_platform();
                return XST_FAILURE;
            }
            if (XAxiDma_Busy(&dma1, XAXIDMA_DEVICE_TO_DMA) && --t1 == 0U) {
                xil_printf("ERROR: DMA1 S2MM timed out\r\n");
                cleanup_platform();
                return XST_FAILURE;
            }
        }

        /* Step 4 — Invalidate D-cache so CPU reads DMA-written data from DDR.
         *          Invalidate (not Flush) is correct for S2MM: the DMA wrote
         *          to DDR; the CPU cache is stale and must be discarded.
         *          Flush would push CPU cache → DDR, which is the wrong
         *          direction and risks overwriting the DMA data.             */
        Xil_DCacheInvalidateRange((UINTPTR)e_buf, DMA_TRANSFER_SIZE);
        Xil_DCacheInvalidateRange((UINTPTR)y_buf, DMA_TRANSFER_SIZE);

        /* Step 5 — For each sample: update TTC PWM with y(n), print e(n)   */
        for (u32 i = 0U; i < SAMPLES_PER_TRANSFER; i++) {

            union { u32 u; float f; } fb;

            fb.u = e_buf[i];  float e  = fb.f;
            fb.u = y_buf[i];  float yn = fb.f;

            /* --- Update TTC PWM duty cycle for this y(n) sample ---
             *
             * Map y(n) ∈ [-1, 1] → duty8 ∈ [0, 255]:
             *   y = -1 → duty8 =   0  (full negative swing)
             *   y =  0 → duty8 = 127  (silence / 50 %)
             *   y = +1 → duty8 = 255  (full positive swing)
             *
             * Scale duty8 to the TTC match range [0, g_ttc_period]:
             *   match = duty8 × g_ttc_period / 255                         */
            float yn_c = yn;
            if (yn_c >  1.0f) yn_c =  1.0f;
            if (yn_c < -1.0f) yn_c = -1.0f;

            u8 duty8 = (u8)((yn_c + 1.0f) * 127.5f);   /* 0 – 255          */

            XInterval match = (XInterval)(
                (u32)duty8 * (u32)g_ttc_period / 255U);

            XTtcPs_SetMatchValue(&ttc, 0, match);

            /* --- UART monitoring ---
             * d_in is recovered exactly: e = d_in - y  →  d_in = e + y.
             * x_in is not a DMA output and cannot be recovered from e/y.     */
            float d_in = e + yn;

            xil_printf("[%3u] ", (unsigned int)i);
            print_float("d_in", d_in);
            xil_printf("  ");
            print_float("e(n)", e);
            xil_printf("  ");
            print_float("y(n)", yn);
            xil_printf("  duty=%3u\r\n", (unsigned int)duty8);
        }

        /* Step 6 — Batch separator                                          */
        xil_printf("--- batch %u | %u samples | LMS autonomous ---\r\n\r\n",
                   (unsigned int)batch,
                   (unsigned int)SAMPLES_PER_TRANSFER);
        batch++;

        usleep(LOOP_DELAY_US);
    }

    cleanup_platform();
    return 0;
}
