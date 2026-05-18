<script lang="ts">
  import type { Album } from '../lib/api'
  import { api } from '../lib/api'
  import { Disc3 } from 'lucide-svelte'

  let {
    albums = [],
    loading = false,
    onSelect,
  }: {
    albums?: Album[]
    loading?: boolean
    onSelect: (album: Album) => void
  } = $props()
</script>

{#if loading}
  <div class="grid grid-cols-[repeat(auto-fill,minmax(160px,1fr))] gap-5">
    {#each Array(12) as _, i (i)}
      <div class="glass animate-pulse p-3">
        <div class="aspect-square rounded-xl bg-white/[0.04]"></div>
        <div class="mt-3 h-3 w-3/4 rounded bg-white/10"></div>
        <div class="mt-2 h-2.5 w-1/2 rounded bg-white/[0.06]"></div>
      </div>
    {/each}
  </div>
{:else if albums.length === 0}
  <div class="glass flex flex-col items-center gap-3 py-20 text-white/40">
    <Disc3 size={40} />
    <p>No albums in the library.</p>
  </div>
{:else}
  <div class="grid grid-cols-[repeat(auto-fill,minmax(160px,1fr))] gap-5">
    {#each albums as album (album.id)}
      <button
        class="glass group p-3 text-left transition hover:-translate-y-1 hover:bg-white/[0.12]"
        onclick={() => onSelect(album)}
      >
        <div
          class="relative aspect-square overflow-hidden rounded-xl bg-white/[0.04] ring-1 ring-white/10"
        >
          <img
            src={api.albumArtUrl(album.id)}
            alt=""
            draggable="false"
            class="h-full w-full object-cover"
            onerror={(e) => { (e.currentTarget as HTMLImageElement).style.display = 'none' }}
          />
          <!-- Placeholder shown when no art loads -->
          <div
            class="absolute inset-0 -z-10 grid place-items-center bg-gradient-to-br from-white/[0.06] to-transparent"
          >
            <Disc3 size={42} class="text-white/20" />
          </div>
        </div>
        <div class="mt-3 truncate text-sm font-semibold text-white/90">
          {album.title || 'Unknown album'}
        </div>
        <div class="truncate text-xs text-white/50">
          {album.artist || 'Unknown artist'}
        </div>
        <div class="mt-1 text-[11px] text-white/35">
          {album.track_count} track{album.track_count === 1 ? '' : 's'}
          {#if album.year}· {album.year}{/if}
        </div>
      </button>
    {/each}
  </div>
{/if}
