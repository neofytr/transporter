<script lang="ts">
  import { queueStore, snapshot } from '../lib/stores'
  import { parseFormat } from '../lib/ws'
  import { api } from '../lib/api'
  import QueueList from '../components/QueueList.svelte'
  import { fmtDuration } from '../lib/utils'
  import { Trash2 } from 'lucide-svelte'

  let q = $derived($queueStore)
  let items = $derived(q?.tracks ?? [])
  let currentIndex = $derived(q?.current_index ?? -1)

  let sampleRate = $derived(
    parseFormat($snapshot?.format_match.matched)?.rate ||
      $snapshot?.source.sample_rate_hz ||
      44100,
  )

  let totalFrames = $derived(
    items.reduce((s, it) => s + (it.duration_frames || 0), 0),
  )

  async function refetch() {
    try {
      const res = await api.getQueue()
      $queueStore = res
    } catch {
      /* offline -- the polling loop in App will retry */
    }
  }

  async function clearAll() {
    try {
      await api.clearQueue()
    } catch {
      /* offline */
    }
    refetch()
  }
</script>

<div class="mx-auto flex h-full max-w-4xl flex-col gap-5 p-6">
  <header class="flex items-center justify-between">
    <div>
      <h1 class="text-2xl font-bold text-white">Queue</h1>
      <p class="mt-1 text-sm text-white/45">
        {items.length} track{items.length === 1 ? '' : 's'}
        {#if totalFrames > 0}
          · {fmtDuration(totalFrames, sampleRate)} total
        {/if}
      </p>
    </div>
    {#if items.length > 0}
      <button
        class="flex items-center gap-2 rounded-full border border-white/15 bg-white/[0.06] px-4 py-2 text-sm text-white/70 transition hover:border-rose-400/40 hover:bg-rose-500/15 hover:text-rose-200"
        onclick={clearAll}
      >
        <Trash2 size={15} /> Clear queue
      </button>
    {/if}
  </header>

  <div class="min-h-0 flex-1 overflow-y-auto pr-1">
    <QueueList
      {items}
      {currentIndex}
      {sampleRate}
      onMutated={refetch}
    />
  </div>
</div>
