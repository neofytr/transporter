// Dominant-color extraction wrapper around colorthief's synchronous browser
// API. Prefers the vivid "Vibrant" semantic swatch and clamps it into an
// HSL range that stays legible against the dark glass surfaces.

import { getSwatchesSync, getColorSync, type SwatchRole } from 'colorthief'

const FALLBACK = '#6366f1'

// Preference order: a saturated swatch first, then progressively less vivid
// fallbacks, so the accent reads with energy whenever the art allows it.
const ROLE_ORDER: SwatchRole[] = [
  'Vibrant',
  'LightVibrant',
  'DarkVibrant',
  'Muted',
  'LightMuted',
  'DarkMuted',
]

function clampForDarkUi(h: number, s: number, l: number): string {
  // h,s,l from colorthief's HSL() are in [0,360], [0,100], [0,100].
  const adjS = Math.min(100, Math.max(45, s))
  const adjL = Math.min(68, Math.max(45, l))
  return hslToHex(h, adjS, adjL)
}

function hslToHex(h: number, s: number, l: number): string {
  s /= 100
  l /= 100
  const k = (n: number) => (n + h / 30) % 12
  const a = s * Math.min(l, 1 - l)
  const f = (n: number) => {
    const c = l - a * Math.max(-1, Math.min(k(n) - 3, Math.min(9 - k(n), 1)))
    return Math.round(255 * c)
  }
  const h2 = (v: number) => v.toString(16).padStart(2, '0')
  return `#${h2(f(0))}${h2(f(8))}${h2(f(4))}`
}

export function extractAccent(img: HTMLImageElement): string {
  try {
    const swatches = getSwatchesSync(img, { colorCount: 8, quality: 10 })
    for (const role of ROLE_ORDER) {
      const sw = swatches[role]
      if (sw) {
        const { h, s, l } = sw.color.hsl()
        return clampForDarkUi(h, s, l)
      }
    }

    // No semantic swatch resolved -- fall back to the raw dominant color.
    const dominant = getColorSync(img, { quality: 10 })
    if (dominant) {
      const { h, s, l } = dominant.hsl()
      return clampForDarkUi(h, s, l)
    }
  } catch {
    /* extraction failed -- keep the default accent */
  }
  return FALLBACK
}
