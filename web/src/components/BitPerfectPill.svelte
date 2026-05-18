<script lang="ts">
  import type { SnapBitPerfect } from '../lib/ws'
  import { ShieldCheck, ShieldAlert, ShieldX, Info } from 'lucide-svelte'

  let { verdict }: { verdict: SnapBitPerfect | null } = $props()

  let level = $derived(verdict?.level ?? 'No')

  const styles = {
    Yes: {
      cls: 'bg-emerald-500/20 text-emerald-300 border-emerald-400/40',
      icon: ShieldCheck,
      label: 'BIT-PERFECT',
    },
    Qualified: {
      cls: 'bg-amber-500/20 text-amber-300 border-amber-400/40',
      icon: ShieldAlert,
      label: 'QUALIFIED',
    },
    No: {
      cls: 'bg-rose-500/20 text-rose-300 border-rose-400/40',
      icon: ShieldX,
      label: 'NOT BIT-PERFECT',
    },
  } as const

  let s = $derived(styles[level])
  let open = $state(false)
  let hasNotes = $derived((verdict?.qualifications.length ?? 0) > 0)
</script>

<div class="relative inline-block">
  {#if verdict}
    {@const Icon = s.icon}
    <button
      class="flex items-center gap-2 rounded-full border px-3.5 py-1.5 text-xs font-bold tracking-wide {s.cls}"
      onmouseenter={() => (open = true)}
      onmouseleave={() => (open = false)}
      onfocus={() => (open = true)}
      onblur={() => (open = false)}
    >
      <Icon size={15} />
      <span>{s.label}</span>
      {#if hasNotes}
        <Info size={12} class="opacity-60" />
      {/if}
    </button>

    {#if open && (hasNotes || level !== 'Yes')}
      <div
        class="glass-strong absolute left-0 top-full z-50 mt-2 w-72 p-3 text-left text-xs"
      >
        <div class="mb-2 font-semibold text-white/90">
          {level === 'Yes' ? 'Bit-perfect path' : 'Why not fully bit-perfect'}
        </div>
        {#if hasNotes}
          <ul class="list-disc space-y-1 pl-4 text-white/65">
            {#each verdict.qualifications as q, i (i)}
              <li>{q}</li>
            {/each}
          </ul>
        {:else}
          <p class="text-white/55">
            The engine reports no qualifying conditions for this verdict.
          </p>
        {/if}
      </div>
    {/if}
  {:else}
    <span
      class="flex items-center gap-2 rounded-full border border-white/15 bg-white/5 px-3.5 py-1.5 text-xs font-bold tracking-wide text-white/40"
    >
      <ShieldAlert size={15} />
      <span>NO SIGNAL</span>
    </span>
  {/if}
</div>
