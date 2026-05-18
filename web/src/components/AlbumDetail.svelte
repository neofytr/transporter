<script lang="ts">
  import type { Album, LibTrack } from '../lib/api'
  import { api } from '../lib/api'
  import CoverArt from './CoverArt.svelte'
  import { fmtTime } from '../lib/utils'
  import { X, Play, Plus, Disc3, ListPlus } from 'lucide-svelte'

  let {
    album,
    onClose,
  }: {
    album: Album
    onClose: () => void
  } = $props()

  let tracks = $state<LibTrack[]>([])
  let loading = $state(true)
  let error = $state(false)
  let busyId = $state<number | null>(null)

  $effect(() => {
    const id = album.id
    loading = true
    error = false
    api
      .getAlbumTracks(id)
      .then((t) => {
        tracks = t
      })
      .catch(() => {
        error = true
      })
      .finally(() => {
        loading = false
      })
  })

  let firstArtId = $derived(tracks.length > 0 ? tracks[0].id : null)
  let totalFrames = $derived(
    tracks.reduce((s, t) => s + (t.duration_frames || 0), 0),
  )
  let sampleRate = 44100

  async function act(fn: () => Promise<void>, id: number) {
    busyId = id
    try {
      await fn()
    } catch {
      /* offline -- ignored */
    } finally {
      busyId = null
    }
  }

  function playTrack(t: LibTrack) {
    act(async () => {
      await api.appendToQueue(t.path)
    }, t.id)
  }

  function appendTrack(t: LibTrack) {
    act(() => api.appendToQueue(t.path), t.id)
  }

  async function appendAll() {
    for (const t of tracks) {
      try {
        await api.appendToQueue(t.path)
      } catch {
        break
      }
    }
  }
</script>

<div
  class="fixed inset-0 z-40 flex items-end justify-center bg-black/60 backdrop-blur-sm"
  role="button"
  tabindex="-1"
  onclick={(e) => {
    if (e.target === e.currentTarget) onClose()
  }}
  onkeydown={(e) => e.key === 'Escape' && onClose()}
>
  <div
    class="glass-strong relative flex h-[88vh] w-full max-w-5xl flex-col overflow-hidden"
    style="animation: slideup 0.32s cubic-bezier(0.16,1,0.3,1);"
  >
    <button
      class="absolute right-4 top-4 z-10 rounded-full bg-white/10 p-2 text-white/70 transition hover:bg-white/20 hover:text-white"
      onclick={onClose}
      aria-label="Close"
    >
      <X size={18} />
    </button>

    <div class="flex gap-6 border-b border-white/10 p-6">
      <div class="w-44 shrink-0">
        <CoverArt track_id={firstArtId} size="large" />
      </div>
      <div class="flex min-w-0 flex-col justify-end">
        <div class="text-xs uppercase tracking-widest text-white/40">
          Album
        </div>
        <h2 class="mt-1 truncate text-3xl font-bold text-white">
          {album.title || 'Unknown album'}
        </h2>
        <div class="mt-1 text-lg text-white/60">
          {album.artist || 'Unknown artist'}
        </div>
        <div class="mt-2 text-sm text-white/40">
          {album.track_count} track{album.track_count === 1 ? '' : 's'}
          {#if album.year}· {album.year}{/if}
          {#if totalFrames > 0}· {fmtTime(totalFrames, sampleRate)}{/if}
        </div>
        <button
          class="mt-4 flex w-fit items-center gap-2 rounded-full bg-accent px-5 py-2 text-sm font-semibold text-black transition hover:brightness-110"
          onclick={appendAll}
        >
          <ListPlus size={16} /> Queue album
        </button>
      </div>
    </div>

    <div class="min-h-0 flex-1 overflow-y-auto p-4">
      {#if loading}
        <div class="space-y-2">
          {#each Array(8) as _, i (i)}
            <div class="h-11 animate-pulse rounded-lg bg-white/[0.04]"></div>
          {/each}
        </div>
      {:else if error}
        <div class="py-16 text-center text-white/40">
          Could not load tracks. Is the backend running?
        </div>
      {:else if tracks.length === 0}
        <div
          class="flex flex-col items-center gap-3 py-16 text-white/40"
        >
          <Disc3 size={32} />
          <p>No tracks.</p>
        </div>
      {:else}
        <ul>
          {#each tracks as t (t.id)}
            <li
              class="group flex items-center gap-4 rounded-lg px-3 py-2.5 transition hover:bg-white/[0.07]"
            >
              <span
                class="w-6 text-right text-sm tabular-nums text-white/35"
              >
                {t.track_number || '-'}
              </span>
              <div class="min-w-0 flex-1">
                <div class="truncate text-sm text-white/90">
                  {t.title || t.path.split('/').pop()}
                </div>
                <div class="truncate text-xs text-white/45">
                  {t.artist || 'Unknown artist'}
                </div>
              </div>
              <span class="text-xs tabular-nums text-white/40">
                {fmtTime(t.duration_frames, sampleRate)}
              </span>
              <div class="flex gap-1 opacity-0 transition group-hover:opacity-100">
                <button
                  class="rounded-full p-2 text-white/70 transition hover:bg-accent hover:text-black disabled:opacity-40"
                  onclick={() => playTrack(t)}
                  disabled={busyId === t.id}
                  aria-label="Play"
                  title="Append + play"
                >
                  <Play size={15} fill="currentColor" />
                </button>
                <button
                  class="rounded-full p-2 text-white/70 transition hover:bg-white/15 hover:text-white disabled:opacity-40"
                  onclick={() => appendTrack(t)}
                  disabled={busyId === t.id}
                  aria-label="Append to queue"
                  title="Append to queue"
                >
                  <Plus size={15} />
                </button>
              </div>
            </li>
          {/each}
        </ul>
      {/if}
    </div>
  </div>
</div>

<style>
  @keyframes slideup {
    from {
      transform: translateY(6%);
      opacity: 0;
    }
    to {
      transform: translateY(0);
      opacity: 1;
    }
  }
</style>
