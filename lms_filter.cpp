#include "ap_axi_sdata.h"
#include "hls_stream.h"

#define N_TAPS  32
#define MU      0.01f

// 16-bit input  — matches XADC AXI-Stream output
typedef ap_axiu<16, 0, 0, 0> adc_sample_t;
// 32-bit output — IEEE-754 float
typedef ap_axiu<32, 0, 0, 0> float_sample_t;

void lms_filter(hls::stream<adc_sample_t>   &s_axis,    // from XADC (interleaved x,d pairs)
                hls::stream<float_sample_t> &m_axis_e,  // e(n) only → DMA0 S2MM
                hls::stream<float_sample_t> &m_axis_y)  // y(n) only → DMA1 S2MM → TTC PWM
{
#pragma HLS INTERFACE axis         port=s_axis
#pragma HLS INTERFACE axis         port=m_axis_e
#pragma HLS INTERFACE axis         port=m_axis_y
#pragma HLS INTERFACE ap_ctrl_none port=return

    static float weights[N_TAPS];
    static float x_buf[N_TAPS];
#pragma HLS ARRAY_PARTITION variable=weights complete
#pragma HLS ARRAY_PARTITION variable=x_buf   complete

    // --- Read one interleaved pair from XADC sequencer ---
    adc_sample_t xs = s_axis.read();
    adc_sample_t ds = s_axis.read();

    float x_in = (float)(ap_uint<16>)xs.data / 65536.0f;
    float d_in = (float)(ap_uint<16>)ds.data / 65536.0f;

    // --- Shift delay line ---
    shift_loop: for (int i = N_TAPS - 1; i > 0; i--) {
#pragma HLS PIPELINE II=1
        x_buf[i] = x_buf[i - 1];
    }
    x_buf[0] = x_in;

    // --- FIR output ---
    float y = 0.0f;
    fir_loop: for (int i = 0; i < N_TAPS; i++) {
#pragma HLS PIPELINE
        y += weights[i] * x_buf[i];
    }

    // --- Error signal ---
    float e = d_in - y;

    // --- Weight update ---
    update_loop: for (int i = 0; i < N_TAPS; i++) {
#pragma HLS PIPELINE II=1
        weights[i] += MU * e * x_buf[i];
    }

    float_sample_t out;
    union { float f; ap_uint<32> u; } fb;

    // Propagate TLAST from the second input sample (ds) to both outputs.
    // The upstream axis_subset_converter fires TLAST every 256 XADC transactions
    // (= every 128 LMS iterations = SAMPLES_PER_TRANSFER).  Because the LMS
    // always reads exactly two samples per invocation, TLAST always lands on
    // the second read (ds), never the first (xs).  Both DMAs have store-and-
    // forward enabled, so they require TLAST to flush their on-chip FIFOs to
    // DDR; without it they stall forever.
    ap_uint<1> tlast = ds.last;

    // --- e(n) → DMA0 S2MM ---
    fb.f = e; out.data = fb.u; out.keep = 0xF; out.last = tlast;
    m_axis_e.write(out);

    // --- y(n) → DMA1 S2MM (ARM reads → TTC PWM duty cycle) ---
    fb.f = y; out.data = fb.u; out.keep = 0xF; out.last = tlast;
    m_axis_y.write(out);
}
