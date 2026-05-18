<script lang="ts">
  import { api } from '../lib/api'
  import { fmtTime } from '../lib/utils'

  let {
    position_frames = 0,
    total_frames = 0,
    sample_rate = 44100,
  }: {
    position_frames?: number
    total_frames?: number
    sample_rate?: number
  } = $props()

  let trackEl = $state<HTMLDivElement | null>(null)
  let dragging = $state(false)
  let dragFrac = $state(0)

  let liveFrac = $derived(
    total_frames > 0
      ? Math.min(1, Math.max(0, position_frames / total_frames))
      : 0,
  )

  let frac = $derived(dragging ? dragFrac : liveFrac)

  let shownPos = $derived(
    dragging ? Math.round(dragFrac * total_frames) : position_frames,
  )

  function fracFromEvent(clientX: number): number {
    if (!trackEl) return 0
    const r = trackEl.getBoundingClientRect()
    return Math.min(1, Math.max(0, (clientX - r.left) / r.width))
  }

  function onPointerDown(e: PointerEvent) {
    if (total_frames <= 0) return
    dragging = true
    dragFrac = fracFromEvent(e.clientX)
    ;(e.currentTarget as HTMLElement).setPointerCapture(e.pointerId)
  }

  function onPointerMove(e: PointerEvent) {
    if (!dragging) return
    dragFrac = fracFromEvent(e.clientX)
  }

  async function onPointerUp(e: PointerEvent) {
    if (!dragging) return
    dragging = false
    const target = Math.round(fracFromEvent(e.clientX) * total_frames)
    try {
      await api.seek(target)
    } catch {
      /* backend offline -- next snapshot will resync */
    }
  }
</script>

<div class="flex w-full items-center gap-3 no-select">
  <span class="w-12 shrink-0 text-right text-xs tabular-nums text-white/60">
    {fmtTime(shownPos, sample_rate)}
  </span>

  <div
    bind:this={trackEl}
    class="group relative h-6 flex-1 cursor-pointer"
    role="slider"
    tabindex="0"
    aria-label="Seek"
    aria-valuemin={0}
    aria-valuemax={total_frames}
    aria-valuenow={shownPos}
    onpointerdown={onPointerDown}
    onpointermove={onPointerMove}
    onpointerup={onPointerUp}
    onpointercancel={() => (dragging = false)}
  >
    <div
      class="absolute inset-x-0 top-1/2 h-1.5 -translate-y-1/2 overflow-hidden rounded-full bg-white/15"
    >
      <div
        class="h-full rounded-full bg-accent"
        style="width: {frac * 100}%; transition: {dragging
          ? 'none'
          : 'width 0.2s linear'}; box-shadow: 0 0 12px rgb(var(--accent) / 0.7);"
      ></div>
    </div>
    <div
      class="absolute top-1/2 h-3.5 w-3.5 -translate-x-1/2 -translate-y-1/2 rounded-full bg-white opacity-0 shadow-lg shadow-black/50 transition-opacity group-hover:opacity-100 {dragging
        ? 'opacity-100'
        : ''}"
      style="left: {frac * 100}%; transition: {dragging
        ? 'opacity 0.15s'
        : 'left 0.2s linear, opacity 0.15s'};"
    ></div>
  </div>

  <span class="w-12 shrink-0 text-xs tabular-nums text-white/60">
    {fmtTime(total_frames, sample_rate)}
  </span>
</div>
