import { useEffect, useRef, useState, type KeyboardEvent, type PointerEvent, type RefObject } from 'react'

export interface ChartPoint {
  /** The bucket start, epoch milliseconds. */
  ms: number
  min: number
  mean: number
  max: number
}

export interface AxisTick {
  ms: number
  label: string
}

export interface ClimateChartProps {
  title: string
  unit: string
  points: ChartPoint[]
  fromMs: number
  toMs: number
  bucketMs: number
  ticks: AxisTick[]
  /** The y axis steps in this unit; the domain snaps outward to it. */
  step: number
  /** Clamps the y domain, e.g. 0–100 for humidity. */
  floor?: number
  ceiling?: number
  /** A CSS color for the band and line. */
  color: string
  hover: number | null
  onHover: (index: number | null) => void
}

const HEIGHT = 170
const FALLBACK_WIDTH = 640
const MARGIN = { top: 10, right: 12, bottom: 24, left: 40 }

function useWidth(): [RefObject<HTMLDivElement | null>, number] {
  const ref = useRef<HTMLDivElement | null>(null)
  const [width, setWidth] = useState(FALLBACK_WIDTH)
  useEffect(() => {
    const element = ref.current
    if (element === null || typeof ResizeObserver === 'undefined') {
      return
    }
    const observer = new ResizeObserver((entries) => {
      const measured = entries[0]?.contentRect.width ?? 0
      if (measured > 0) {
        setWidth(measured)
      }
    })
    observer.observe(element)
    return () => {
      observer.disconnect()
    }
  }, [])
  return [ref, width]
}

/** Splits `points` wherever a bucket is missing, so a sensor outage reads as a gap and never as a straight line. */
export function runsOf(points: ChartPoint[], bucketMs: number): ChartPoint[][] {
  const runs: ChartPoint[][] = []
  let current: ChartPoint[] = []
  for (const point of points) {
    const previous = current[current.length - 1]
    if (previous !== undefined && point.ms - previous.ms > bucketMs * 1.5) {
      runs.push(current)
      current = []
    }
    current.push(point)
  }
  if (current.length > 0) {
    runs.push(current)
  }
  return runs
}

/** Returns the index of the point whose bucket centre lies nearest `ms`. `points` is in time order. */
export function nearestIndex(points: ChartPoint[], bucketMs: number, ms: number): number {
  let low = 0
  let high = points.length - 1
  while (low < high) {
    const middle = Math.floor((low + high) / 2)
    if (points[middle]!.ms + bucketMs / 2 < ms) {
      low = middle + 1
    } else {
      high = middle
    }
  }
  const before = points[low - 1]
  const at = points[low]!
  if (before !== undefined && Math.abs(before.ms + bucketMs / 2 - ms) <= Math.abs(at.ms + bucketMs / 2 - ms)) {
    return low - 1
  }
  return low
}

export default function ClimateChart(props: ClimateChartProps) {
  const { title, unit, points, fromMs, toMs, bucketMs, ticks, step, color, hover, onHover } = props
  const [wrapper, width] = useWidth()

  const plotWidth = Math.max(width - MARGIN.left - MARGIN.right, 10)
  const plotHeight = HEIGHT - MARGIN.top - MARGIN.bottom

  const lowest = Math.min(...points.map((point) => point.min))
  const highest = Math.max(...points.map((point) => point.max))
  let yLow = Math.floor(lowest / step) * step
  let yHigh = Math.ceil(highest / step) * step
  if (props.floor !== undefined) {
    yLow = Math.max(yLow, props.floor)
  }
  if (props.ceiling !== undefined) {
    yHigh = Math.min(yHigh, props.ceiling)
  }
  if (yHigh <= yLow) {
    yHigh = yLow + step
  }

  const x = (ms: number) => MARGIN.left + ((ms - fromMs) / (toMs - fromMs)) * plotWidth
  const y = (value: number) => MARGIN.top + (1 - (value - yLow) / (yHigh - yLow)) * plotHeight
  const centre = (point: ChartPoint) => Math.min(Math.max(point.ms + bucketMs / 2, fromMs), toMs)

  const yTicks: number[] = []
  for (let value = yLow; value <= yHigh + step / 1000; value += step) {
    yTicks.push(value)
  }

  const runs = runsOf(points, bucketMs)
  const bands = runs.map((run) => {
    const upper = run.map((point) => `${x(centre(point)).toFixed(1)},${y(point.max).toFixed(1)}`)
    const lower = [...run].reverse().map((point) => `${x(centre(point)).toFixed(1)},${y(point.min).toFixed(1)}`)
    return `M${upper.join('L')}L${lower.join('L')}Z`
  })
  const lines = runs.map(
    (run) => `M${run.map((point) => `${x(centre(point)).toFixed(1)},${y(point.mean).toFixed(1)}`).join('L')}`,
  )
  const lone = runs.filter((run) => run.length === 1).map((run) => run[0]!)

  const selected = hover === null ? undefined : points[hover]

  const onPointerMove = (event: PointerEvent<SVGRectElement>) => {
    const box = event.currentTarget.getBoundingClientRect()
    if (box.width <= 0 || points.length === 0) {
      return
    }
    const ms = fromMs + ((event.clientX - box.left) / box.width) * (toMs - fromMs)
    onHover(nearestIndex(points, bucketMs, ms))
  }

  const onKeyDown = (event: KeyboardEvent<SVGSVGElement>) => {
    if (points.length === 0) {
      return
    }
    const last = points.length - 1
    const current = hover ?? last
    const next =
      event.key === 'ArrowLeft'
        ? Math.max(current - 1, 0)
        : event.key === 'ArrowRight'
          ? Math.min(current + 1, last)
          : event.key === 'Home'
            ? 0
            : event.key === 'End'
              ? last
              : null
    if (next !== null) {
      event.preventDefault()
      onHover(next)
    }
  }

  return (
    <figure className="climate-chart">
      <figcaption className="climate-chart__title">
        {title} <span className="climate-chart__unit">{unit}</span>
      </figcaption>
      <div ref={wrapper} className="climate-chart__plot">
        <svg
          width={width}
          height={HEIGHT}
          viewBox={`0 0 ${width} ${HEIGHT}`}
          role="img"
          aria-label={`${title}, ${Math.round(lowest)} to ${Math.round(highest)} ${unit}`}
          tabIndex={0}
          onKeyDown={onKeyDown}
          onBlur={() => {
            onHover(null)
          }}
        >
          {yTicks.map((value) => (
            <g key={value}>
              <line
                className="climate-chart__grid"
                x1={MARGIN.left}
                x2={MARGIN.left + plotWidth}
                y1={y(value)}
                y2={y(value)}
              />
              <text className="climate-chart__tick" x={MARGIN.left - 6} y={y(value)} textAnchor="end" dominantBaseline="middle">
                {value}
              </text>
            </g>
          ))}
          {ticks.map((tick) => (
            <text
              key={tick.ms}
              className="climate-chart__tick"
              x={x(tick.ms)}
              y={HEIGHT - 6}
              textAnchor="middle"
            >
              {tick.label}
            </text>
          ))}
          {bands.map((path, index) => (
            <path key={`band-${index}`} d={path} fill={color} fillOpacity={0.16} stroke="none" />
          ))}
          {lines.map((path, index) => (
            <path
              key={`line-${index}`}
              d={path}
              fill="none"
              stroke={color}
              strokeWidth={2}
              strokeLinejoin="round"
              strokeLinecap="round"
            />
          ))}
          {lone.map((point) => (
            <circle key={`lone-${point.ms}`} cx={x(centre(point))} cy={y(point.mean)} r={2} fill={color} />
          ))}
          {selected === undefined ? null : (
            <g data-testid="climate-crosshair">
              <line
                className="climate-chart__crosshair"
                x1={x(centre(selected))}
                x2={x(centre(selected))}
                y1={MARGIN.top}
                y2={MARGIN.top + plotHeight}
              />
              <circle
                cx={x(centre(selected))}
                cy={y(selected.mean)}
                r={4}
                fill={color}
                stroke="var(--chart-surface)"
                strokeWidth={2}
              />
            </g>
          )}
          <rect
            x={MARGIN.left}
            y={MARGIN.top}
            width={plotWidth}
            height={plotHeight}
            fill="transparent"
            style={{ touchAction: 'pan-y' }}
            onPointerMove={onPointerMove}
            onPointerDown={onPointerMove}
            onPointerLeave={() => {
              onHover(null)
            }}
          />
        </svg>
      </div>
    </figure>
  )
}
