<script lang="ts">
  import { api } from '../lib/api'
  import { playerState } from '../lib/stores'
  import { SkipBack, SkipForward, Play, Pause } from 'lucide-svelte'

  let isPlaying = $derived($playerState?.state === 'Playing')
  let busy = $state(false)

  async function guard(fn: () => Promise<void>) {
    if (busy) return
    busy = true
    try {
      await fn()
    } catch {
      /* backend offline -- snapshot will resync state */
    } finally {
      busy = false
    }
  }

  function toggle() {
    guard(() => (isPlaying ? api.pause() : api.play()))
  }

  function prev() {
    const idx = $playerState?.queue_index ?? 0
    if (idx > 0) guard(() => api.jumpToQueue(idx - 1))
  }

  function next() {
    const ps = $playerState
    if (ps && ps.queue_index + 1 < ps.queue_size) {
      guard(() => api.jumpToQueue(ps.queue_index + 1))
    }
  }
</script>

<div class="flex items-center justify-center gap-5 no-select">
  <button
    class="grid h-11 w-11 place-items-center rounded-full text-white/80 transition hover:bg-white/10 hover:text-white active:scale-95 disabled:opacity-30"
    onclick={prev}
    disabled={($playerState?.queue_index ?? 0) <= 0}
    aria-label="Previous track"
  >
    <SkipBack size={22} fill="currentColor" />
  </button>

  <button
    class="grid h-16 w-16 place-items-center rounded-full bg-accent text-black shadow-lg shadow-accent/40 transition hover:brightness-110 active:scale-95"
    onclick={toggle}
    aria-label={isPlaying ? 'Pause' : 'Play'}
  >
    {#if isPlaying}
      <Pause size={28} fill="currentColor" />
    {:else}
      <Play size={28} fill="currentColor" class="ml-0.5" />
    {/if}
  </button>

  <button
    class="grid h-11 w-11 place-items-center rounded-full text-white/80 transition hover:bg-white/10 hover:text-white active:scale-95 disabled:opacity-30"
    onclick={next}
    disabled={!$playerState ||
      $playerState.queue_index + 1 >= $playerState.queue_size}
    aria-label="Next track"
  >
    <SkipForward size={22} fill="currentColor" />
  </button>
</div>
