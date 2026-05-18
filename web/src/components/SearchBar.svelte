<script lang="ts">
  import { Search, X } from 'lucide-svelte'

  let {
    onSearch,
    placeholder = 'Search tracks, artists, albums...',
    debounceMs = 300,
  }: {
    onSearch: (query: string) => void
    placeholder?: string
    debounceMs?: number
  } = $props()

  let value = $state('')
  let timer: ReturnType<typeof setTimeout> | null = null

  function schedule(q: string) {
    if (timer) clearTimeout(timer)
    timer = setTimeout(() => {
      timer = null
      onSearch(q.trim())
    }, debounceMs)
  }

  function onInput(e: Event) {
    value = (e.target as HTMLInputElement).value
    schedule(value)
  }

  function clear() {
    value = ''
    if (timer) clearTimeout(timer)
    onSearch('')
  }
</script>

<div class="glass flex items-center gap-3 px-4 py-3">
  <Search size={18} class="shrink-0 text-white/40" />
  <input
    type="text"
    {placeholder}
    bind:value
    oninput={onInput}
    class="w-full bg-transparent text-sm text-white placeholder:text-white/35 focus:outline-none"
  />
  {#if value}
    <button
      class="shrink-0 rounded-full p-1 text-white/40 transition hover:bg-white/10 hover:text-white"
      onclick={clear}
      aria-label="Clear search"
    >
      <X size={15} />
    </button>
  {/if}
</div>
