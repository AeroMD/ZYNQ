#!/usr/bin/env python3
"""
lms_flowchart.py
Generates a clean, Word-ready LMS algorithm flowchart.
Output: lms_flowchart.png  (300 dpi, A4-friendly width)
"""

import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

fig, ax = plt.subplots(figsize=(9, 11))
ax.set_xlim(0, 9)
ax.set_ylim(0.2, 12.1)
ax.axis('off')
fig.patch.set_facecolor('white')

# ── Colours ───────────────────────────────────────────────────────────────────
C_INPUT  = '#BBDEFB'   # light blue    – inputs
C_CALC   = '#ECEFF1'   # light grey    – computation steps
C_UPDATE = '#FFE0B2'   # light orange  – weight update
C_OUTPUT = '#C8E6C9'   # light green   – outputs
C_ARROW  = '#546E7A'   # soft dark grey
C_TEXT   = '#212121'   # near-black text (readable on all light backgrounds)

# ── Helper: draw a labelled box ───────────────────────────────────────────────
# border colours that match each fill (one shade darker)
BORDER = {
    '#BBDEFB': '#1565C0',   # input  – blue
    '#ECEFF1': '#78909C',   # calc   – grey
    '#FFE0B2': '#E65100',   # update – orange
    '#C8E6C9': '#2E7D32',   # output – green
}

def draw_box(cx, cy, w, h, lines, color, fontsize=9.5):
    box = FancyBboxPatch(
        (cx - w/2, cy - h/2), w, h,
        boxstyle='round,pad=0.12',
        facecolor=color,
        edgecolor=BORDER.get(color, '#90A4AE'),
        linewidth=1.8, zorder=3
    )
    ax.add_patch(box)
    text = '\n'.join(lines)
    ax.text(cx, cy, text,
            ha='center', va='center',
            fontsize=fontsize, color=C_TEXT,
            fontweight='bold', zorder=4,
            multialignment='center',
            linespacing=1.5)

# ── Helper: vertical arrow ────────────────────────────────────────────────────
def draw_arrow(x, y_start, y_end, label=''):
    ax.annotate('',
        xy=(x, y_end), xytext=(x, y_start),
        arrowprops=dict(arrowstyle='->', color=C_ARROW,
                        lw=1.8, mutation_scale=16),
        zorder=2)
    if label:
        ax.text(x + 0.12, (y_start + y_end) / 2, label,
                fontsize=8, color='#616161',
                va='center', style='italic')

# ── Layout: boxes from top to bottom ─────────────────────────────────────────
cx  = 4.2    # centre x — shifted left, loop arrow no longer clips
bw  = 5.6    # box width
bh  = 0.68   # box height
gap = 0.50   # more breathing room

positions = []
y = 11.3
steps = [
    # (lines,                                        color)
    (['START — new sample pair arrives'],             C_CALC),
    (['STEP 1',
      'Read  x(n)  from Rx "ref"  →  VP/VN'],       C_INPUT),
    (['STEP 2',
      'Read  d(n)  from Rx "observer"  →  VAUXP0'], C_INPUT),
    (['STEP 3  —  Shift delay line',
      'x_buf = [ x(n),  x(n-1),  …,  x(n-31) ]'],  C_CALC),
    (['STEP 4  —  FIR filter output',
      'y(n)  =  Σ  w[i] × x_buf[i]   (i = 0…31)'], C_CALC),
    (['STEP 5  —  Compute error',
      'e(n)  =  d(n)  −  y(n)'],                    C_CALC),
    (['STEP 6  —  Update weights',
      'w[i]  =  w[i]  +  μ × e(n) × x_buf[i]',
      'μ  =  0.01   (learning rate)'],               C_UPDATE),
    (['STEP 7  —  Output results',
      'y(n)  →  Tx "feedback"  (anti-noise speaker)',
      'e(n)  →  ARM processor  (UART monitor)'],     C_OUTPUT),
    (['WAIT for next sample pair',
      'then repeat from STEP 1'],                    C_CALC),
]

# taller boxes for 3-line entries, same for 1 and 2 line entries
heights = [bh, bh, bh, bh, bh, bh, bh * 1.5, bh * 1.5, bh]

for (lines, color), h in zip(steps, heights):
    positions.append((cx, y, h))
    draw_box(cx, y, bw, h, lines, color,
             fontsize=9.5 if len(lines) == 1 else 9.0)
    y -= (h + gap)

# ── Arrows between boxes ──────────────────────────────────────────────────────
for i in range(len(positions) - 1):
    _, y_curr, h_curr = positions[i]
    _, y_next, h_next = positions[i + 1]
    y_start = y_curr - h_curr / 2
    y_end   = y_next + h_next / 2
    draw_arrow(cx, y_start - 0.02, y_end + 0.02)

# ── Loop-back arrow from last box to second box (STEP 1) ─────────────────────
_, y_last, h_last = positions[-1]
_, y_first_step, h_first_step = positions[1]   # STEP 1

loop_x = cx + bw/2 + 0.65    # fully clear of boxes
y_loop_start = y_last - h_last/2
y_loop_end   = y_first_step + h_first_step/2

# Horizontal stub out from last box
ax.annotate('', xy=(loop_x, y_loop_start),
            xytext=(cx + bw/2, y_loop_start),
            arrowprops=dict(arrowstyle='-', color=C_ARROW, lw=1.8))
# Vertical line up
ax.plot([loop_x, loop_x], [y_loop_start, y_loop_end],
        color=C_ARROW, lw=1.8, linestyle='dashed', zorder=2)
# Horizontal arrow back into STEP 1
ax.annotate('',
    xy=(cx + bw/2, y_loop_end),
    xytext=(loop_x, y_loop_end),
    arrowprops=dict(arrowstyle='->', color=C_ARROW,
                    lw=1.8, mutation_scale=18),
    zorder=2)
ax.text(loop_x + 0.12, (y_loop_start + y_loop_end) / 2,
        'repeat\nper sample', fontsize=8.5, color='#424242',
        va='center', ha='left', fontweight='bold')

# ── Title ─────────────────────────────────────────────────────────────────────
fig.suptitle('LMS Adaptive Filter — Algorithm Flowchart',
             fontsize=14, fontweight='bold', color='#212121',
             y=0.98)

# ── Legend ────────────────────────────────────────────────────────────────────
legend_items = [
    mpatches.Patch(color=C_INPUT,  label='ADC Input'),
    mpatches.Patch(color=C_CALC,   label='Computation'),
    mpatches.Patch(color=C_UPDATE, label='Weight Update'),
    mpatches.Patch(color=C_OUTPUT, label='Output'),
]
ax.legend(handles=legend_items, loc='lower center',
          ncol=4, fontsize=9,
          frameon=True, framealpha=0.95,
          edgecolor='#BDBDBD',
          bbox_to_anchor=(0.5, 0.01))

plt.tight_layout(rect=[0, 0.04, 1, 0.96])   # reserve space for suptitle + legend
fig.savefig('lms_flowchart.png', dpi=300,
            bbox_inches='tight', facecolor='white')
print('Saved: lms_flowchart.png')
plt.show()
