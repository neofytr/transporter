// Typed REST client for the transporter daemon. Dev hits the Vite proxy at the
// same origin (which forwards /api -> localhost:7800); production is served by
// the C++ binary itself, also same-origin. So a relative base works for both.

const BASE = ''

const TOKEN_KEY = 'transporter_token'

export function getToken(): string {
  return localStorage.getItem(TOKEN_KEY) ?? ''
}

export function setToken(token: string): void {
  localStorage.setItem(TOKEN_KEY, token)
}

export function hasToken(): boolean {
  return localStorage.getItem(TOKEN_KEY) !== null
}

export interface PlayerState {
  state: string
  current_track: Track | null
  position_frames: number
  queue_index: number
  queue_size: number
}

export interface Track {
  path: string
  title: string
  artist: string
  album: string
  duration_frames: number
  format: string
}

export interface QueueItem {
  index: number
  path: string
  title?: string
  artist?: string
  duration_frames?: number
  track_id?: number
}

export interface Album {
  id: number
  title: string
  artist: string
  year: number
  track_count: number
}

export interface LibTrack {
  id: number
  path: string
  title: string
  artist: string
  album?: string
  album_id?: number
  track_number: number
  duration_frames: number
  format: string
}

export interface DeviceInfo {
  id: string
  alsa_hw_string: string
  display_name: string
  is_usb: boolean
  active: boolean
  caps_probe_failed: boolean
  probe_failure_reason: string
  formats: string[]
  sample_rates: number[]
}

interface QueueResponse {
  tracks: QueueItem[]
  current_index: number
}

class ApiError extends Error {
  status: number
  constructor(status: number, message: string) {
    super(message)
    this.status = status
    this.name = 'ApiError'
  }
}

async function request<T>(
  method: 'GET' | 'POST',
  path: string,
  body?: unknown,
): Promise<T> {
  const headers: Record<string, string> = {}
  // The daemon gates every /api/ route except the static shell and cover art,
  // so the bearer token must ride on GETs as well as POSTs.
  const token = getToken()
  if (token) headers['Authorization'] = `Bearer ${token}`
  if (method === 'POST') {
    headers['Content-Type'] = 'application/json'
  }

  const res = await fetch(BASE + path, {
    method,
    headers,
    body: body !== undefined ? JSON.stringify(body) : undefined,
  })

  if (!res.ok) {
    let detail = res.statusText
    try {
      const txt = await res.text()
      if (txt) detail = txt
    } catch {
      /* ignore parse failure */
    }
    throw new ApiError(res.status, detail)
  }

  const ct = res.headers.get('content-type') ?? ''
  if (ct.includes('application/json')) {
    return (await res.json()) as T
  }
  return undefined as T
}

export const api = {
  getState(): Promise<PlayerState> {
    return request<PlayerState>('GET', '/api/state')
  },

  play(): Promise<void> {
    return request<void>('POST', '/api/play')
  },

  pause(): Promise<void> {
    return request<void>('POST', '/api/pause')
  },

  seek(frame: number): Promise<void> {
    return request<void>('POST', '/api/seek', { frame: Math.max(0, Math.round(frame)) })
  },

  load(path: string): Promise<void> {
    return request<void>('POST', '/api/load', { path })
  },

  getQueue(): Promise<QueueResponse> {
    return request<QueueResponse>('GET', '/api/queue')
  },

  appendToQueue(path: string): Promise<void> {
    return request<void>('POST', '/api/queue/append', { path })
  },

  removeFromQueue(index: number): Promise<void> {
    return request<void>('POST', '/api/queue/remove', { index })
  },

  reorderQueue(from: number, to: number): Promise<void> {
    return request<void>('POST', '/api/queue/reorder', { from, to })
  },

  clearQueue(): Promise<void> {
    return request<void>('POST', '/api/queue/clear')
  },

  jumpToQueue(index: number): Promise<void> {
    return request<void>('POST', '/api/queue/jump', { index })
  },

  getAlbums(): Promise<Album[]> {
    return request<Album[]>('GET', '/api/library/albums')
  },

  getAlbumTracks(albumId: number): Promise<LibTrack[]> {
    return request<LibTrack[]>('GET', `/api/library/tracks?album_id=${albumId}`)
  },

  search(query: string): Promise<LibTrack[]> {
    return request<LibTrack[]>(
      'GET',
      `/api/library/search?q=${encodeURIComponent(query)}`,
    )
  },

  getDevices(): Promise<DeviceInfo[]> {
    return request<DeviceInfo[]>('GET', '/api/devices')
  },

  selectDevice(alsa_hw_string: string): Promise<{ ok: boolean; restart_required: boolean }> {
    return request('POST', '/api/devices/select', { alsa_hw_string })
  },

  artUrl(trackId: number): string {
    return `${BASE}/api/art/${trackId}`
  },
}

export { ApiError }
