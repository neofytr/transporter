<script lang="ts">
  import { api, type Album, type LibTrack } from '../lib/api'
  import SearchBar from '../components/SearchBar.svelte'
  import AlbumGrid from '../components/AlbumGrid.svelte'
  import AlbumDetail from '../components/AlbumDetail.svelte'
  import { fmtTime } from '../lib/utils'
  import { Plus } from 'lucide-svelte'
  import { onMount } from 'svelte'

  let albums = $state<Album[]>([])
  let loading = $state(true)
  let selected = $state<Album | null>(null)

  let searchActive = $state(false)
  let searchResults = $state<LibTrack[]>([])
  let searching = $state(false)

  async function loadAlbums() {
    loading = true
    try {
      albums = await api.getAlbums()
    } catch {
      albums = []
    } finally {
      loading = false
    }
  }

  onMount(loadAlbums)

  async function onSearch(q: string) {
    if (!q) {
      searchActive = false
      searchResults = []
      return
    }
    searchActive = true
    searching = true
    try {
      searchResults = await api.search(q)
    } catch {
      searchResults = []
    } finally {
      searching = false
    }
  }

  async function append(t: LibTrack) {
    try {
      await api.appendToQueue(t.path)
    } catch {
      /* offline */
    }
  }
</script>

<div class="mx-auto flex h-full max-w-6xl flex-col gap-5 p-6">
  <header class="flex items-center justify-between">
    <h1 class="text-2xl font-bold text-white">Library</h1>
    <span class="text-sm text-white/40">
      {albums.length} album{albums.length === 1 ? '' : 's'}
    </span>
  </header>

  <SearchBar {onSearch} />

  <div class="min-h-0 flex-1 overflow-y-auto pr-1">
    {#if searchActive}
      {#if searching}
        <div class="space-y-2">
          {#each Array(8) as _, i (i)}
            <div class="h-14 animate-pulse rounded-xl bg-white/[0.04]"></div>
          {/each}
        </div>
      {:else if searchResults.length === 0}
        <div class="glass py-16 text-center text-white/40">
          No matches.
        </div>
      {:else}
        <ul class="space-y-1.5">
          {#each searchResults as t (t.id)}
            <li
              class="glass group flex items-center gap-4 px-4 py-3 transition hover:bg-white/[0.12]"
            >
              <div class="min-w-0 flex-1">
                <div class="truncate text-sm font-medium text-white/90">
                  {t.title || t.path.split('/').pop()}
                </div>
                <div class="truncate text-xs text-white/45">
                  {t.artist || 'Unknown artist'}
                  {#if t.album}· {t.album}{/if}
                </div>
              </div>
              <span class="text-xs tabular-nums text-white/40">
                {fmtTime(t.duration_frames, 44100)}
              </span>
              <button
                class="rounded-full p-2 text-white/60 opacity-0 transition hover:bg-accent hover:text-black group-hover:opacity-100"
                onclick={() => append(t)}
                aria-label="Append to queue"
                title="Append to queue"
              >
                <Plus size={16} />
              </button>
            </li>
          {/each}
        </ul>
      {/if}
    {:else}
      <AlbumGrid {albums} {loading} onSelect={(a) => (selected = a)} />
    {/if}
  </div>
</div>

{#if selected}
  <AlbumDetail album={selected} onClose={() => (selected = null)} />
{/if}
