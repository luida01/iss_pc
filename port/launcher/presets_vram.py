# SPDX-License-Identifier: GPL-2.0-or-later
"""The VRAM presets: what to use on a card that cannot hold the game's whole working set.

Measured on 2026-10-19 (RTX 3050 Laptop, 4 GB, 720p, ISS_GC_BUDGET_MB=3500, evidence
20261019-130735-g0-manual-paint): nvidia-smi held 3848 MB of the card's 4096 MB and the port's
own accounting said 4757 MiB (the driver counts the host memory of the buffer cache's arenas
too, so its number is above the card and cannot be compared with a VRAM budget). The texture
collector compared that total with the budget, so it stayed in its aggressive mode for the
whole session and evicted the textures the game was drawing with (up to 334 images in one 5 s
report, 3771 in 30 minutes): every eviction is a re-upload from the guest memory, which may
hold another texture by then. That is the detail the player sees missing, and the 305 MB of
textures re-uploaded in a single frame (evidence 20261019-140307-g0-manual-paint3).

The budget of 3500 MB is the measured working point of a 4 GB card: the card gives 4096 MB and
the rest (the desktop, the driver and the buffer cache's resident guest memory, ~600 MB) has to
fit in what is left. The texture cache held ~3078 MiB in that session, so the budget is a cap
the cache never reaches: the collector has nothing to do and the textures stay where they are.

The two presets give the player the two ends of the quality trade-off:

- BALANCED 4 GB: the measured working point (3500 MB of budget), the game's own texture bias
  and the precise GPU reads (the GPU particles need them).
- DETAIL: the same budget (the working set fits in it either way) with a negative texture LOD
  bias and more sharpening: the textures are sharper at a distance, which is what a player who
  says "the detail is missing" is asking for.

Both keep the game's own draw distance (a shorter one would remove the distant particles the
player is missing) and the precise GPU reads.
"""

BALANCED_4GB = {
    'id': 'balanced_4gb',
    'exported': '2026-10-19',
    'names': {'en': 'BALANCED 4 GB', 'pt': 'EQUILIBRADO 4 GB'},
    'what': {
        'en': 'For a card with 4 GB of VRAM: the texture cache gets the 3500 MB a 4 GB card can '
              'afford (the measured working point), the texture LOD bias stays at the game\'s own '
              'value and the GPU reads stay precise (the particles need them). The collector has '
              'nothing to free, so the textures stay where they are: no re-uploads, no missing '
              'detail.',
        'pt': 'Para uma placa com 4 GB de VRAM: a cache de texturas fica com os 3500 MB que uma '
              'placa de 4 GB aguenta (o ponto medido), o bias de LOD das texturas fica no valor '
              'do próprio jogo e as leituras da GPU continuam precisas (as partículas precisam). '
              'O coletor não tem nada para liberar, então as texturas ficam onde estão: sem '
              'recargas, sem detalhe faltando.',
    },
    # launcher settings (settings.json): the GPU accuracy (the particles need the precise reads)
    # and the VRAM budget, the one machine-specific key a preset of this kind owns.
    'settings': {'vsync': True, 'gpu_readbacks': '2', 'eager_readback': False, 'write_merge': True,
                 'precompile_shaders': True, 'show_fps': True, 'vram_budget_mb': 3500},
    'items': {
        'menu:fsr1': '0',        # FSR 1 blurs the image (the game renders below the window size)
        'menu:rcas': '1',
        'menu:rcas_strength': '60',
        'menu:lod_scale': '1',   # the game's own draw distance: the distant particles stay
        'menu:region_lod': '1',
        'menu:region_fade': '1',
        'menu:near_clip': '10',
        'menu:far_clip': '10000000',
        'menu:mip_lod_bias': '0',
        'TLG_STALE_READBACK': '1',
        'TLG_DEFERRED_READBACK': '1',
    },
    'not_applied': {
        'display_mode': 'Windowed', 'engine_fps': 120,
        'block1d': 'auto (1 on this PC)', 'bc_tiny': 'auto (1 on this PC)', 'no_msaa': 'auto (1 on this PC)',
    },
}

DETAIL = {
    'id': 'detail',
    'exported': '2026-10-19',
    'names': {'en': 'DETAIL', 'pt': 'DETALHE'},
    'what': {
        'en': 'The textures are the picture: the same 3500 MB of budget (the working set fits in '
              'it, so the VRAM is the same) but a negative texture LOD bias (sharper at a '
              'distance) and more sharpening. Costs a little GPU time, not VRAM.',
        'pt': 'As texturas são a imagem: os mesmos 3500 MB de orçamento (o conjunto de trabalho '
              'cabe nele, então a VRAM é a mesma) mas com bias de LOD das texturas negativo (mais '
              'nítido ao longe) e mais nitidez. Custa um pouco de tempo de GPU, não VRAM.',
    },
    'settings': {'vsync': True, 'gpu_readbacks': '2', 'eager_readback': False, 'write_merge': True,
                 'precompile_shaders': True, 'show_fps': True, 'vram_budget_mb': 3500},
    'items': {
        'menu:fsr1': '0',
        'menu:rcas': '1',
        'menu:rcas_strength': '70',
        'menu:lod_scale': '1',
        'menu:region_lod': '1',
        'menu:region_fade': '1',
        'menu:near_clip': '10',
        'menu:far_clip': '10000000',
        'menu:mip_lod_bias': '-0.5',
        'TLG_STALE_READBACK': '1',
        'TLG_DEFERRED_READBACK': '1',
    },
    'not_applied': {
        'display_mode': 'Windowed', 'engine_fps': 120,
        'block1d': 'auto (1 on this PC)', 'bc_tiny': 'auto (1 on this PC)', 'no_msaa': 'auto (1 on this PC)',
    },
}

VRAM_PRESETS = (BALANCED_4GB, DETAIL)
