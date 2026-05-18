<script lang="ts">
  import { api, type DeviceInfo } from '../lib/api'
  import { Cpu, Usb, AlertTriangle, X, CheckCircle2 } from 'lucide-svelte'

  let {
    onClose,
  }: {
    onClose: () => void
  } = $props()

  let devices = $state<DeviceInfo[]>([])
  let loading = $state(true)
  let error = $state(false)

  $effect(() => {
    api
      .getDevices()
      .then((d) => {
        devices = d
      })
      .catch(() => {
        error = true
      })
      .finally(() => {
        loading = false
      })
  })

  function rateLabel(rates: number[]): string {
    if (rates.length === 0) return '—'
    const khz = rates.map((r) => (r / 1000).toFixed(1).replace('.0', ''))
    return khz.join(' / ') + ' kHz'
  }
</script>

<div
  class="fixed inset-0 z-50 flex items-start justify-center"
  role="button"
  tabindex="-1"
  onclick={(e) => {
    if (e.target === e.currentTarget) onClose()
  }}
  onkeydown={(e) => e.key === 'Escape' && onClose()}
>
  <div
    class="glass-strong relative mt-20 w-full max-w-lg overflow-hidden"
    style="animation: fadedown 0.22s cubic-bezier(0.16,1,0.3,1);"
  >
    <div class="flex items-center justify-between border-b border-white/10 px-5 py-4">
      <span class="text-sm font-semibold text-white">Playback Devices</span>
      <button
        class="rounded-full p-1.5 text-white/50 transition hover:bg-white/10 hover:text-white"
        onclick={onClose}
        aria-label="Close"
      >
        <X size={15} />
      </button>
    </div>

    <div class="max-h-[60vh] overflow-y-auto p-3">
      {#if loading}
        <div class="space-y-2 p-2">
          {#each Array(3) as _, i (i)}
            <div class="h-16 animate-pulse rounded-xl bg-white/[0.04]"></div>
          {/each}
        </div>
      {:else if error}
        <div class="py-10 text-center text-sm text-white/40">
          Could not enumerate devices.
        </div>
      {:else if devices.length === 0}
        <div class="py-10 text-center text-sm text-white/40">
          No playback devices found.
        </div>
      {:else}
        <ul class="space-y-1.5">
          {#each devices as dev (dev.id)}
            <li
              class="relative rounded-xl border px-4 py-3 transition
                {dev.active
                  ? 'border-accent/40 bg-accent/10'
                  : 'border-white/[0.07] bg-white/[0.03] hover:bg-white/[0.06]'}"
            >
              <div class="flex items-start gap-3">
                <div class="mt-0.5 shrink-0 text-white/40">
                  {#if dev.is_usb}
                    <Usb size={16} class={dev.active ? 'text-accent' : ''} />
                  {:else}
                    <Cpu size={16} class={dev.active ? 'text-accent' : ''} />
                  {/if}
                </div>

                <div class="min-w-0 flex-1">
                  <div class="flex items-center gap-2">
                    <span
                      class="truncate text-sm font-medium
                        {dev.active ? 'text-white' : 'text-white/80'}"
                    >
                      {dev.display_name}
                    </span>
                    {#if dev.active}
                      <CheckCircle2 size={13} class="shrink-0 text-accent" />
                    {/if}
                  </div>

                  <div class="mt-0.5 font-mono text-[10px] text-white/35">
                    {dev.alsa_hw_string}
                  </div>

                  {#if dev.caps_probe_failed}
                    <div class="mt-1.5 flex items-center gap-1.5 text-xs text-amber-400/80">
                      <AlertTriangle size={11} />
                      <span>
                        {dev.probe_failure_reason || 'Device busy or unavailable'}
                      </span>
                    </div>
                  {:else}
                    <div class="mt-1.5 text-[11px] text-white/40">
                      {dev.formats.join(' · ')}
                      {#if dev.sample_rates.length > 0}
                        &nbsp;·&nbsp;{rateLabel(dev.sample_rates)}
                      {/if}
                    </div>
                  {/if}
                </div>
              </div>
            </li>
          {/each}
        </ul>
      {/if}
    </div>

    <div class="border-t border-white/10 px-5 py-3 text-[11px] text-white/30">
      Device selection is configured via <span class="font-mono">preferred</span> in
      <span class="font-mono">transporter.toml</span>.
    </div>
  </div>
</div>

<style>
  @keyframes fadedown {
    from {
      transform: translateY(-6px);
      opacity: 0;
    }
    to {
      transform: translateY(0);
      opacity: 1;
    }
  }
</style>
