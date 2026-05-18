// WebSocket telemetry client. The daemon pushes a PipelineSnapshot at 10 Hz on
// /api/snapshot. Reconnects with exponential backoff (capped at 5 s) and never
// surfaces transport errors to the UI -- a dropped backend just looks like
// "no snapshot yet" upstream.
//
// Note: the browser WebSocket API cannot set an Authorization header, so the
// telemetry stream is only reachable when the daemon runs without a token.
// With auth enabled the handshake is rejected and the silent reconnect loop
// keeps retrying -- the rest of the UI still works over the REST client,
// which does send the bearer token. Token-gated WS would need server support
// for a query-param or subprotocol credential.
//
// The shapes below mirror exactly what src/web/server.cpp serializes, which is
// a deliberately flattened projection of the engine's PipelineSnapshot struct:
// the format stages are emitted as "<rate>/<FMT>/<ch>" strings rather than
// nested objects, and several audit fields are not on the wire yet.

export interface SnapSource {
  file_path: string
  codec_name: string
  sample_rate_hz: number
  channels: number
  bit_depth_file: number
  total_frames: number
  bitrate_kbps: number
}

export interface SnapDecoder {
  thread_state: string
  frames_produced: number
}

export interface SnapFormatMatch {
  matched_ok: boolean
  declared: string // "44100/S16_LE/2"
  matched: string
  rejection_reason: string
}

export interface SnapRing {
  capacity_bytes: number
  fill_bytes: number
  fill_frames: number
  fill_us: number
}

export interface SnapOutput {
  period_size_frames: number
  periods: number
  xrun_count: number
  frames_written: number
  gapless_pending: boolean
}

export interface SnapDevice {
  current_hw_string: string
  is_connected: boolean
}

export interface SnapRealtime {
  mode: string // "FIFO" | "OTHER"
  fallback_reason: string
}

export type BitPerfectLevel = 'Yes' | 'Qualified' | 'No'

export interface SnapBitPerfect {
  level: BitPerfectLevel
  qualifications: string[]
}

export interface PipelineSnapshot {
  engine_state: string
  source: SnapSource
  decoder: SnapDecoder
  format_match: SnapFormatMatch
  ring: SnapRing
  output: SnapOutput
  device: SnapDevice
  realtime: SnapRealtime
  bit_perfect: SnapBitPerfect
}

// "<rate>/<FMT>/<channels>" -> { rate, fmt, channels }. Returns null for the
// engine's idle sentinel ("0/...") so callers can render an em dash.
export function parseFormat(
  s: string | undefined,
): { rate: number; fmt: string; channels: number } | null {
  if (!s) return null
  const parts = s.split('/')
  if (parts.length !== 3) return null
  const rate = Number(parts[0])
  const channels = Number(parts[2])
  if (!rate) return null
  return { rate, fmt: parts[1], channels }
}

export function formatLabel(s: string | undefined): string {
  const p = parseFormat(s)
  if (!p) return '—'
  return `${p.fmt} · ${(p.rate / 1000).toFixed(1)} kHz · ${p.channels}ch`
}

const MAX_BACKOFF_MS = 5000
const BASE_BACKOFF_MS = 250

export function connectSnapshot(
  onMessage: (snap: PipelineSnapshot) => void,
): () => void {
  let ws: WebSocket | null = null
  let backoff = BASE_BACKOFF_MS
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null
  let closed = false

  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
  const url = `${proto}//${location.host}/api/snapshot`

  function scheduleReconnect() {
    if (closed || reconnectTimer) return
    const delay = backoff
    backoff = Math.min(backoff * 2, MAX_BACKOFF_MS)
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null
      open()
    }, delay)
  }

  function open() {
    if (closed) return
    try {
      ws = new WebSocket(url)
    } catch {
      scheduleReconnect()
      return
    }

    ws.onopen = () => {
      backoff = BASE_BACKOFF_MS
    }

    ws.onmessage = (ev) => {
      try {
        const snap = JSON.parse(ev.data as string) as PipelineSnapshot
        onMessage(snap)
      } catch {
        /* malformed frame -- ignore, next frame is 100 ms away */
      }
    }

    ws.onerror = () => {
      // The close handler runs the reconnect; swallow the error event so it
      // never reaches the console as an uncaught rejection.
      try {
        ws?.close()
      } catch {
        /* already closing */
      }
    }

    ws.onclose = () => {
      if (!closed) scheduleReconnect()
    }
  }

  open()

  return () => {
    closed = true
    if (reconnectTimer) {
      clearTimeout(reconnectTimer)
      reconnectTimer = null
    }
    if (ws) {
      ws.onclose = null
      ws.onerror = null
      ws.onmessage = null
      try {
        ws.close()
      } catch {
        /* ignore */
      }
      ws = null
    }
  }
}
