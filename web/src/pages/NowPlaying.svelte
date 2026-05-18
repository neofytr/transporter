<script lang="ts">
  import { playerState, snapshot } from '../lib/stores'
  import { parseFormat } from '../lib/ws'
  import { api } from '../lib/api'
  import CoverArt from '../components/CoverArt.svelte'
  import Seekbar from '../components/Seekbar.svelte'
  import TransportControls from '../components/TransportControls.svelte'
  import { onMount } from 'svelte'

  let ps = $derived($playerState)
  let snap = $derived($snapshot)

  let track = $derived(ps?.current_track ?? null)

  // Sample rate drives time formatting; prefer the live matched rate, fall
  // back to source rate, then 44.1k.
  let matchedFmt = $derived(parseFormat(snap?.format_match.matched))
  let sampleRate = $derived(
    matchedFmt?.rate || snap?.source.sample_rate_hz || 44100,
  )

  // The state/queue feed does not carry a track id; the art endpoint is keyed
  // by library track id. The pipeline source stage doesn't expose it either,
  // so cover art on Now Playing falls back to the placeholder unless the
  // backend later threads an id through. Color extraction still works off the
  // placeholder-free path when an id is present.
  let artId = $derived<number | null>(null)

  let format = $derived(
    matchedFmt
      ? `${matchedFmt.fmt} · ${(matchedFmt.rate / 1000).toFixed(1)} kHz · ${
          matchedFmt.channels
        }ch`
      : (track?.format ?? ''),
  )

  let position = $derived(ps?.position_frames ?? 0)
  let total = $derived(
    track?.duration_frames || snap?.source.total_frames || 0,
  )

  async function onKey(e: KeyboardEvent) {
    if (e.target instanceof HTMLInputElement) return
    if (e.code === 'Space') {
      e.preventDefault()
      try {
        if (ps?.state === 'Playing') await api.pause()
        else await api.play()
      } catch {
        /* offline */
      }
    } else if (e.code === 'ArrowLeft') {
      e.preventDefault()
      try {
        await api.seek(Math.max(0, position - 10 * sampleRate))
      } catch {
        /* offline */
      }
    } else if (e.code === 'ArrowRight') {
      e.preventDefault()
      try {
        await api.seek(Math.min(total || position, position + 10 * sampleRate))
      } catch {
        /* offline */
      }
    }
  }

  onMount(() => {
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  })
</script>

<div class="relative h-full w-full overflow-hidden">
  <!-- Full-bleed blurred backdrop -->
  <div class="absolute inset-0">
    {#if artId != null}
      <img
        src={api.artUrl(artId)}
        alt=""
        aria-hidden="true"
        class="h-full w-full object-cover"
        style="filter: blur(40px) brightness(0.4) saturate(1.2); transform: scale(1.1);"
      />
    {:else}
      <div
        class="h-full w-full"
        style="background: radial-gradient(60rem 60rem at 30% 20%, rgb(var(--accent) / 0.35), transparent 60%), #07070b; filter: brightness(0.85);"
      ></div>
    {/if}
    <div class="absolute inset-0 bg-black/30"></div>
  </div>

  <div class="relative z-10 flex h-full items-center justify-center p-8">
    <div
      class="glass-strong flex w-full max-w-4xl flex-col gap-8 p-8 md:flex-row md:items-center"
      style="box-shadow: 0 0 0 1px rgb(var(--accent) / 0.18), 0 30px 80px -20px rgb(var(--accent) / 0.35);"
    >
      <div class="mx-auto w-64 shrink-0 md:mx-0">
        <CoverArt track_id={artId} size="large" extractColor={true} />
      </div>

      <div class="flex min-w-0 flex-1 flex-col gap-6">
        <div class="min-w-0">
          {#if track}
            <h1 class="truncate text-4xl font-bold leading-tight text-white">
              {track.title || track.path.split('/').pop()}
            </h1>
            <p class="mt-2 truncate text-xl text-white/70">
              {track.artist || 'Unknown artist'}
            </p>
            <p class="mt-1 truncate text-sm text-white/50">
              {track.album || ''}
            </p>
          {:else}
            <h1 class="text-3xl font-bold text-white/70">Nothing playing</h1>
            <p class="mt-2 text-sm text-white/40">
              Queue a track from the Library.
            </p>
          {/if}

          {#if format}
            <span
              class="mt-4 inline-block rounded-full border border-white/15 bg-white/[0.07] px-3 py-1 font-mono text-xs text-white/70"
            >
              {format}
            </span>
          {/if}
        </div>

        <Seekbar
          position_frames={position}
          total_frames={total}
          sample_rate={sampleRate}
        />

        <TransportControls />

        <div
          class="flex justify-center gap-6 text-[11px] uppercase tracking-wider text-white/35"
        >
          <span>Space play/pause</span>
          <span>&larr; -10s</span>
          <span>&rarr; +10s</span>
        </div>
      </div>
    </div>
  </div>
</div>
