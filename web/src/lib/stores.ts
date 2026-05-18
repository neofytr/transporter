import { writable, type Writable } from 'svelte/store'
import type { PlayerState, QueueItem } from './api'
import type { PipelineSnapshot } from './ws'

export const playerState: Writable<PlayerState | null> = writable(null)

export const queueStore: Writable<{
  tracks: QueueItem[]
  current_index: number
} | null> = writable(null)

export const snapshot: Writable<PipelineSnapshot | null> = writable(null)

// Dominant cover-art color, updated by CoverArt. Hex string. Also mirrored
// into the --accent CSS variable (as space-separated RGB) so Tailwind's
// `accent` color and the gradient wash track it.
export const accentColor: Writable<string> = writable('#6366f1')

export const activeTab: Writable<
  'nowplaying' | 'library' | 'queue' | 'pipeline'
> = writable('nowplaying')

// Connection health: true once any data has arrived from the backend.
export const backendOnline: Writable<boolean> = writable(false)

function hexToRgbTriplet(hex: string): string | null {
  const m = /^#?([0-9a-f]{6})$/i.exec(hex.trim())
  if (!m) return null
  const n = parseInt(m[1], 16)
  return `${(n >> 16) & 255} ${(n >> 8) & 255} ${n & 255}`
}

accentColor.subscribe((hex) => {
  if (typeof document === 'undefined') return
  const triplet = hexToRgbTriplet(hex)
  if (triplet) {
    document.documentElement.style.setProperty('--accent', triplet)
  }
})
