<script lang="ts">
  import { api } from '../lib/api'
  import { accentColor } from '../lib/stores'
  import { extractAccent } from '../lib/colorthief'
  import { Music } from 'lucide-svelte'

  let {
    track_id = null,
    size = 'medium',
    extractColor = false,
    rounded = 'rounded-2xl',
  }: {
    track_id?: number | null
    size?: 'small' | 'medium' | 'large'
    extractColor?: boolean
    rounded?: string
  } = $props()

  const dims = {
    small: 'w-16 h-16',
    medium: 'w-44 h-44',
    large: 'w-full aspect-square',
  }

  let imgEl = $state<HTMLImageElement | null>(null)
  let loaded = $state(false)
  let errored = $state(false)

  let url = $derived(track_id != null ? api.artUrl(track_id) : null)

  // Crossfade whenever the resolved art URL changes.
  $effect(() => {
    url
    loaded = false
    errored = false
  })

  function onLoad() {
    loaded = true
    errored = false
    if (imgEl && extractColor) {
      try {
        $accentColor = extractAccent(imgEl)
      } catch {
        /* keep previous accent */
      }
    }
  }

  function onError() {
    errored = true
    loaded = false
  }
</script>

<div
  class="relative overflow-hidden {rounded} {dims[
    size
  ]} bg-white/[0.04] shadow-2xl shadow-black/60 ring-1 ring-white/10 no-select"
>
  {#if url && !errored}
    <img
      bind:this={imgEl}
      src={url}
      alt="Cover art"
      crossorigin="anonymous"
      draggable="false"
      class="cover-img h-full w-full object-cover {loaded
        ? 'opacity-100'
        : 'scale-105 opacity-0'}"
      onload={onLoad}
      onerror={onError}
    />
  {/if}

  {#if !url || errored || !loaded}
    <div
      class="absolute inset-0 grid place-items-center bg-gradient-to-br from-white/[0.06] to-white/[0.02]"
    >
      <Music
        class="text-white/25"
        size={size === 'small' ? 22 : size === 'medium' ? 48 : 72}
      />
    </div>
  {/if}
</div>

<style>
  /* Crossfade + gentle scale settle on every track change. */
  .cover-img {
    transition:
      opacity 0.45s ease-out,
      transform 0.45s ease-out;
  }
</style>
