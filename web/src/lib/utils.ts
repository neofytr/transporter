import { clsx, type ClassValue } from 'clsx'
import { twMerge } from 'tailwind-merge'

export function cn(...inputs: ClassValue[]): string {
  return twMerge(clsx(inputs))
}

// Frames -> "M:SS" given a sample rate. Falls back to 0:00 when the rate is
// unknown so the UI never prints NaN.
export function fmtTime(frames: number, sampleRate: number): string {
  if (!sampleRate || sampleRate <= 0 || !isFinite(frames) || frames < 0) {
    return '0:00'
  }
  const total = Math.floor(frames / sampleRate)
  const m = Math.floor(total / 60)
  const s = total % 60
  return `${m}:${s.toString().padStart(2, '0')}`
}

// Long-form total used by the queue header: "H:MM:SS" or "M:SS".
export function fmtDuration(frames: number, sampleRate: number): string {
  if (!sampleRate || sampleRate <= 0 || !isFinite(frames) || frames < 0) {
    return '0:00'
  }
  const total = Math.floor(frames / sampleRate)
  const h = Math.floor(total / 3600)
  const m = Math.floor((total % 3600) / 60)
  const s = total % 60
  if (h > 0) {
    return `${h}:${m.toString().padStart(2, '0')}:${s
      .toString()
      .padStart(2, '0')}`
  }
  return `${m}:${s.toString().padStart(2, '0')}`
}

export function fmtBytes(bytes: number): string {
  if (!isFinite(bytes) || bytes < 0) return '0 B'
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`
  return `${(bytes / (1024 * 1024)).toFixed(2)} MiB`
}
