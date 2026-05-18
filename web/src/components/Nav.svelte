<script lang="ts">
  import { activeTab, backendOnline } from '../lib/stores'
  import { Disc3, Library, ListMusic, Activity } from 'lucide-svelte'

  const tabs = [
    { id: 'nowplaying', label: 'Now Playing', icon: Disc3 },
    { id: 'library', label: 'Library', icon: Library },
    { id: 'queue', label: 'Queue', icon: ListMusic },
    { id: 'pipeline', label: 'Pipeline', icon: Activity },
  ] as const
</script>

<nav
  class="glass mx-auto mt-4 flex w-fit items-center gap-1 px-2 py-2 no-select"
>
  {#each tabs as tab (tab.id)}
    {@const Icon = tab.icon}
    <button
      class="flex items-center gap-2 rounded-xl px-4 py-2 text-sm font-medium transition
        {$activeTab === tab.id
        ? 'bg-accent text-black shadow-md shadow-accent/30'
        : 'text-white/60 hover:bg-white/10 hover:text-white'}"
      onclick={() => ($activeTab = tab.id)}
    >
      <Icon size={16} />
      <span>{tab.label}</span>
    </button>
  {/each}

  <div class="ml-2 mr-1 flex items-center gap-1.5 pl-3 border-l border-white/10">
    <span
      class="h-2 w-2 rounded-full {$backendOnline
        ? 'bg-emerald-400'
        : 'bg-amber-400 animate-pulse'}"
      title={$backendOnline ? 'Connected' : 'Connecting...'}
    ></span>
    <span class="text-[11px] text-white/40">
      {$backendOnline ? 'live' : 'connecting'}
    </span>
  </div>
</nav>
