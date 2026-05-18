<script lang="ts">
  import type { QueueItem } from '../lib/api'
  import { api } from '../lib/api'
  import { dndzone, type DndEvent } from 'svelte-dnd-action'
  import { flip } from 'svelte/animate'
  import { fmtTime } from '../lib/utils'
  import { X, Play, GripVertical } from 'lucide-svelte'

  let {
    items = [],
    currentIndex = -1,
    sampleRate = 44100,
    onMutated,
  }: {
    items?: QueueItem[]
    currentIndex?: number
    sampleRate?: number
    onMutated: () => void
  } = $props()

  // Local working copy so the drag library can mutate freely; we reconcile
  // with the backend on drop, then ask the parent to refetch.
  let rows = $state<QueueItem[]>([])
  let dragging = $state(false)

  $effect(() => {
    if (!dragging) {
      rows = items.map((it) => ({ ...it }))
    }
  })

  function handleConsider(e: CustomEvent<DndEvent<QueueItem>>) {
    dragging = true
    rows = e.detail.items
  }

  async function handleFinalize(e: CustomEvent<DndEvent<QueueItem>>) {
    const next = e.detail.items
    rows = next
    dragging = false

    // The backend assigns each item a stable `index`. The original visual
    // order is items[].index; the post-drop order is next[].index. A single
    // drag moves exactly one element: `from` is its position before, `to`
    // its position after. The moved element is the one whose neighbours
    // differ most -- equivalently, the element absent from the longest
    // common prefix/suffix of the two index sequences.
    const before = items.map((it) => it.index)
    const after = next.map((it) => it.index)

    let lo = 0
    while (
      lo < before.length &&
      lo < after.length &&
      before[lo] === after[lo]
    ) {
      lo++
    }
    let hiB = before.length - 1
    let hiA = after.length - 1
    while (hiB >= lo && hiA >= lo && before[hiB] === after[hiA]) {
      hiB--
      hiA--
    }

    // `after[lo..hiA]` is the disturbed window. The moved element is the one
    // present at a different end; resolve from/to via its identity.
    let from = -1
    let to = -1
    for (let i = lo; i <= hiA; i++) {
      const origPos = before.indexOf(after[i])
      if (origPos !== i) {
        from = origPos
        to = i
        break
      }
    }

    if (from >= 0 && to >= 0 && from !== to) {
      try {
        await api.reorderQueue(from, to)
      } catch {
        /* offline -- parent refetch will restore truth */
      }
    }
    onMutated()
  }

  async function remove(it: QueueItem) {
    try {
      await api.removeFromQueue(it.index)
    } catch {
      /* offline */
    }
    onMutated()
  }

  async function jump(it: QueueItem) {
    try {
      await api.jumpToQueue(it.index)
    } catch {
      /* offline */
    }
    onMutated()
  }
</script>

{#if rows.length === 0}
  <div class="glass py-20 text-center text-white/40">Queue is empty.</div>
{:else}
  <ul
    class="space-y-1.5"
    use:dndzone={{
      items: rows,
      flipDurationMs: 200,
      dropTargetStyle: {},
    }}
    onconsider={handleConsider}
    onfinalize={handleFinalize}
  >
    {#each rows as it (it.index)}
      <li animate:flip={{ duration: 200 }} class="list-none">
        <div
          class="glass group flex items-center gap-4 px-4 py-3 transition hover:bg-white/[0.12] {it.index ===
          currentIndex
            ? 'border-l-4 border-l-accent'
            : ''}"
        >
          <span
            class="cursor-grab text-white/25 transition group-hover:text-white/50 active:cursor-grabbing"
            aria-hidden="true"
          >
            <GripVertical size={16} />
          </span>
          <span class="w-7 text-right text-sm tabular-nums text-white/35">
            {it.index + 1}
          </span>
          <div class="min-w-0 flex-1">
            <div
              class="truncate text-sm {it.index === currentIndex
                ? 'font-semibold text-accent'
                : 'text-white/90'}"
            >
              {it.title || it.path.split('/').pop()}
            </div>
            <div class="truncate text-xs text-white/45">
              {it.artist || it.path}
            </div>
          </div>
          {#if it.duration_frames}
            <span class="text-xs tabular-nums text-white/40">
              {fmtTime(it.duration_frames, sampleRate)}
            </span>
          {/if}
          <div class="flex gap-1">
            <button
              class="rounded-full p-2 text-white/60 opacity-0 transition hover:bg-accent hover:text-black group-hover:opacity-100"
              onclick={() => jump(it)}
              aria-label="Play this"
              title="Play this track"
            >
              <Play size={14} fill="currentColor" />
            </button>
            <button
              class="rounded-full p-2 text-white/60 opacity-0 transition hover:bg-rose-500/30 hover:text-rose-200 group-hover:opacity-100"
              onclick={() => remove(it)}
              aria-label="Remove"
              title="Remove from queue"
            >
              <X size={14} />
            </button>
          </div>
        </div>
      </li>
    {/each}
  </ul>
{/if}
