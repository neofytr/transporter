<script lang="ts">
  import { untrack } from 'svelte'
  import { snapshot } from '../lib/stores'
  import { formatLabel, parseFormat } from '../lib/ws'
  import PipelineStageCard from '../components/PipelineStageCard.svelte'
  import BitPerfectPill from '../components/BitPerfectPill.svelte'
  import Sparkline from '../components/Sparkline.svelte'
  import { fmtBytes } from '../lib/utils'
  import { scaleLinear } from 'd3-scale'

  let snap = $derived($snapshot)

  // 60 s of ring fill at 10 Hz = 600 samples.
  const MAX_POINTS = 600
  let ringFill = $state<number[]>([])
  let xrunEvents = $state<number[]>([]) // wall-clock ms of detected xruns
  let lastXrunCount = -1

  $effect(() => {
    const s = $snapshot
    if (!s) return
    const pct =
      s.ring.capacity_bytes > 0
        ? (s.ring.fill_bytes / s.ring.capacity_bytes) * 100
        : 0
    const xc = s.output.xrun_count
    // untrack: writing $state inside an effect that reads $snapshot must not
    // re-trigger the same effect, or it loops at 10 Hz forever.
    untrack(() => {
      ringFill = [...ringFill, pct].slice(-MAX_POINTS)
      if (lastXrunCount >= 0 && xc > lastXrunCount) {
        xrunEvents = [...xrunEvents, Date.now()].slice(-200)
      }
      lastXrunCount = xc
    })
  })

  // Xrun timeline geometry.
  const XW = 100
  const WINDOW_MS = 60_000
  let xrunMarks = $derived.by(() => {
    const now = Date.now()
    const sx = scaleLinear().domain([now - WINDOW_MS, now]).range([0, XW])
    return xrunEvents.filter((t) => t >= now - WINDOW_MS).map((t) => sx(t))
  })

  let ringPct = $derived(
    snap && snap.ring.capacity_bytes > 0
      ? (snap.ring.fill_bytes / snap.ring.capacity_bytes) * 100
      : 0,
  )

  let rtMode = $derived(snap?.realtime.mode ?? '—')
  let matched = $derived(parseFormat(snap?.format_match.matched))
</script>

<div class="mx-auto flex h-full max-w-7xl flex-col p-6">
  <!-- Sticky verdict bar -->
  <div
    class="glass sticky top-0 z-10 mb-5 flex flex-wrap items-center gap-4 px-5 py-3"
  >
    <BitPerfectPill verdict={snap?.bit_perfect ?? null} />

    <div class="flex flex-wrap items-center gap-3 text-sm">
      <span class="font-mono text-white/80">
        {formatLabel(snap?.format_match.matched)}
      </span>
      <span class="text-white/20">|</span>
      <span class="text-white/65">
        DAC: {snap?.device.current_hw_string || '—'}
      </span>
      <span class="text-white/20">|</span>
      <span
        class="rounded-full border px-2.5 py-0.5 text-xs font-semibold {rtMode ===
        'FIFO'
          ? 'border-emerald-400/40 bg-emerald-500/15 text-emerald-300'
          : 'border-amber-400/40 bg-amber-500/15 text-amber-300'}"
      >
        RT: {rtMode === 'FIFO' ? 'SCHED_FIFO' : 'SCHED_OTHER'}
      </span>
    </div>

    <span class="ml-auto text-xs text-white/35">
      engine: {snap?.engine_state ?? 'offline'}
    </span>
  </div>

  <div class="min-h-0 flex-1 overflow-y-auto pr-1">
    {#if !snap}
      <div class="glass py-24 text-center text-white/45">
        Connecting to telemetry stream...
      </div>
    {:else}
      <div class="grid grid-cols-1 gap-4 md:grid-cols-2 xl:grid-cols-3">
        <PipelineStageCard
          title="Source"
          rows={[
            { k: 'File', v: snap.source.file_path || '—', mono: true },
            { k: 'Codec', v: snap.source.codec_name || '—' },
            {
              k: 'Bit depth',
              v: snap.source.bit_depth_file
                ? `${snap.source.bit_depth_file}-bit`
                : '—',
            },
            {
              k: 'Rate',
              v: snap.source.sample_rate_hz
                ? `${(snap.source.sample_rate_hz / 1000).toFixed(1)} kHz`
                : '—',
            },
            { k: 'Channels', v: String(snap.source.channels || '—') },
            {
              k: 'Bitrate',
              v: snap.source.bitrate_kbps
                ? `${snap.source.bitrate_kbps} kbps`
                : 'lossless',
            },
            {
              k: 'Total frames',
              v: snap.source.total_frames.toLocaleString(),
              mono: true,
            },
          ]}
        />

        <PipelineStageCard
          title="Decoder"
          rows={[
            {
              k: 'Frames produced',
              v: snap.decoder.frames_produced.toLocaleString(),
              mono: true,
            },
            {
              k: 'Thread',
              v: snap.decoder.thread_state || 'idle',
              warn: snap.decoder.thread_state === 'error',
            },
          ]}
        />

        <PipelineStageCard
          title="Format Match"
          rows={[
            {
              k: 'Declared',
              v: formatLabel(snap.format_match.declared),
              mono: true,
            },
            {
              k: 'Matched',
              v: formatLabel(snap.format_match.matched),
              mono: true,
            },
            {
              k: 'Result',
              v: snap.format_match.matched_ok ? 'OK' : 'REJECTED',
              warn: !snap.format_match.matched_ok,
            },
            ...(snap.format_match.rejection_reason
              ? [
                  {
                    k: 'Reason',
                    v: snap.format_match.rejection_reason,
                    warn: true,
                  },
                ]
              : []),
          ]}
        />

        <PipelineStageCard
          title="Ring"
          rows={[
            { k: 'Capacity', v: fmtBytes(snap.ring.capacity_bytes) },
            {
              k: 'Fill',
              v: `${fmtBytes(snap.ring.fill_bytes)} (${ringPct.toFixed(0)}%)`,
            },
            {
              k: 'Fill frames',
              v: snap.ring.fill_frames.toLocaleString(),
              mono: true,
            },
            {
              k: 'Fill time',
              v: `${(snap.ring.fill_us / 1000).toFixed(1)} ms`,
              mono: true,
            },
          ]}
        >
          <div class="rounded-lg bg-black/20 p-2">
            <div
              class="mb-1 flex justify-between text-[10px] uppercase tracking-wider text-white/35"
            >
              <span>ring fill %</span>
              <span>60 s</span>
            </div>
            <Sparkline
              values={ringFill}
              width={300}
              height={56}
              domainMax={100}
            />
          </div>
        </PipelineStageCard>

        <PipelineStageCard
          title="Output"
          rows={[
            {
              k: 'Period',
              v: `${snap.output.period_size_frames} fr`,
              mono: true,
            },
            { k: 'Periods', v: String(snap.output.periods), mono: true },
            {
              k: 'Frames written',
              v: snap.output.frames_written.toLocaleString(),
              mono: true,
            },
            {
              k: 'Gapless',
              v: snap.output.gapless_pending ? 'staged' : 'no',
            },
          ]}
        />

        <PipelineStageCard
          title="Xrun"
          rows={[
            {
              k: 'Count (session)',
              v: String(snap.output.xrun_count),
              warn: snap.output.xrun_count > 0,
            },
            {
              k: 'In last 60 s',
              v: String(xrunMarks.length),
              warn: xrunMarks.length > 0,
            },
          ]}
        >
          <div class="rounded-lg bg-black/20 p-2">
            <div
              class="mb-1 flex justify-between text-[10px] uppercase tracking-wider text-white/35"
            >
              <span>xrun events</span>
              <span>60 s</span>
            </div>
            <svg
              viewBox="0 0 {XW} 24"
              preserveAspectRatio="none"
              class="block h-8 w-full"
            >
              <line
                x1="0"
                y1="22"
                x2={XW}
                y2="22"
                stroke="rgba(255,255,255,0.14)"
                stroke-width="0.5"
              />
              {#each xrunMarks as x, i (i)}
                <line
                  x1={x}
                  y1="3"
                  x2={x}
                  y2="22"
                  stroke="#fb7185"
                  stroke-width="0.8"
                />
                <circle cx={x} cy="3" r="1.4" fill="#fb7185" />
              {/each}
            </svg>
          </div>
        </PipelineStageCard>

        <PipelineStageCard
          title="Device"
          accentBar={!snap.device.is_connected}
          rows={[
            {
              k: 'HW string',
              v: snap.device.current_hw_string || '—',
              mono: true,
            },
            {
              k: 'Connected',
              v: snap.device.is_connected ? 'yes' : 'DISCONNECTED',
              warn: !snap.device.is_connected,
            },
          ]}
        />

        <PipelineStageCard
          title="Realtime"
          rows={[
            {
              k: 'Mode',
              v: snap.realtime.mode === 'FIFO' ? 'SCHED_FIFO' : 'SCHED_OTHER',
              warn: snap.realtime.mode !== 'FIFO',
            },
            ...(snap.realtime.fallback_reason
              ? [
                  {
                    k: 'Fallback',
                    v: snap.realtime.fallback_reason,
                    warn: true,
                  },
                ]
              : []),
          ]}
        />
      </div>

      {#if matched}
        <p class="mt-4 text-center text-[11px] text-white/30">
          live pipeline · {matched.fmt} @ {matched.rate} Hz · updated at 10 Hz
        </p>
      {/if}
    {/if}
  </div>
</div>
