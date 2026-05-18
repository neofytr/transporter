<script lang="ts">
  import type { Snippet } from 'svelte'

  let {
    title,
    rows = [],
    accentBar = false,
    children,
  }: {
    title: string
    rows?: Array<{ k: string; v: string; mono?: boolean; warn?: boolean }>
    accentBar?: boolean
    children?: Snippet
  } = $props()
</script>

<div class="glass relative overflow-hidden p-4">
  {#if accentBar}
    <div
      class="absolute inset-y-0 left-0 w-1 bg-accent"
      style="box-shadow: 0 0 16px rgb(var(--accent) / 0.6);"
    ></div>
  {/if}
  <div
    class="mb-3 text-[11px] font-bold uppercase tracking-[0.14em] text-white/45"
  >
    {title}
  </div>

  {#if rows.length > 0}
    <dl class="space-y-1.5">
      {#each rows as row, i (i)}
        <div class="flex items-baseline justify-between gap-3 text-sm">
          <dt class="shrink-0 text-white/45">{row.k}</dt>
          <dd
            class="truncate text-right {row.mono
              ? 'font-mono text-[12.5px]'
              : ''} {row.warn ? 'text-rose-300' : 'text-white/85'}"
            title={row.v}
          >
            {row.v}
          </dd>
        </div>
      {/each}
    </dl>
  {/if}

  {#if children}
    <div class="mt-3">
      {@render children()}
    </div>
  {/if}
</div>
