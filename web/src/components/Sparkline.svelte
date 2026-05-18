<script lang="ts">
  import { scaleLinear } from 'd3-scale'
  import { area, line, curveMonotoneX } from 'd3-shape'
  import { extent } from 'd3-array'

  let {
    values = [],
    width = 240,
    height = 56,
    color = 'rgb(var(--accent))',
    domainMax = undefined,
  }: {
    values?: number[]
    width?: number
    height?: number
    color?: string
    domainMax?: number | undefined
  } = $props()

  let paths = $derived.by(() => {
    if (values.length < 2) return { fill: '', stroke: '' }

    const xs = scaleLinear()
      .domain([0, values.length - 1])
      .range([0, width])

    const [lo, hi] = extent(values) as [number, number]
    const top = domainMax ?? (hi === lo ? hi + 1 : hi)
    const bottom = Math.min(0, lo)

    const ys = scaleLinear()
      .domain([bottom, top])
      .range([height - 1, 1])

    const fill = area<number>()
      .x((_, i) => xs(i))
      .y0(height)
      .y1((d) => ys(d))
      .curve(curveMonotoneX)

    const stroke = line<number>()
      .x((_, i) => xs(i))
      .y((d) => ys(d))
      .curve(curveMonotoneX)

    return {
      fill: fill(values) ?? '',
      stroke: stroke(values) ?? '',
    }
  })

  const gid = `spark-${Math.random().toString(36).slice(2, 9)}`
</script>

<svg
  {width}
  {height}
  viewBox="0 0 {width} {height}"
  preserveAspectRatio="none"
  class="block w-full"
>
  <defs>
    <linearGradient id={gid} x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%" stop-color={color} stop-opacity="0.45" />
      <stop offset="100%" stop-color={color} stop-opacity="0.02" />
    </linearGradient>
  </defs>
  {#if paths.fill}
    <path d={paths.fill} fill="url(#{gid})" />
    <path
      d={paths.stroke}
      fill="none"
      stroke={color}
      stroke-width="1.5"
      stroke-linejoin="round"
      stroke-linecap="round"
    />
  {:else}
    <line
      x1="0"
      y1={height - 1}
      x2={width}
      y2={height - 1}
      stroke="rgba(255,255,255,0.12)"
      stroke-width="1"
    />
  {/if}
</svg>
