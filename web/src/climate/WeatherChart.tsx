import type { KeyboardEvent, PointerEvent } from 'react'
import { useWidth, type AxisTick } from './ClimateChart'

export interface WaterPoint {
  /** The bucket start, epoch milliseconds. */
  ms: number
  rain: number | null
  et0: number | null
}

export interface WeatherChartProps {
  points: WaterPoint[]
  fromMs: number
  toMs: number
  bucketMs: number
  ticks: AxisTick[]
  /** Rain and ET₀ totals over the range, shown in the caption. */
  rainTotal: number
  et0Total: number
  hover: number | null
  onHover: (index: number | null) => void
}

const HEIGHT = 170
const MARGIN = { top: 10, right: 12, bottom: 24, left: 40 }
const STEPS = [0.01, 0.02, 0.05, 0.1, 0.2, 0.25, 0.5, 1, 2, 5]
const GRID_LINES = 4

/** The smallest standard step that covers `highest` in at most four grid lines. */
export function waterStep(highest: number): number {
  return STEPS.find((step) => step * GRID_LINES >= highest) ?? STEPS[STEPS.length - 1]!
}

/** Returns the index of the point whose bucket centre lies nearest `ms`, or null when there are none. */
export function nearestWaterIndex(points: WaterPoint[], bucketMs: number, ms: number): number | null {
  let best: number | null = null
  for (let index = 0; index < points.length; index++) {
    const distance = Math.abs(points[index]!.ms + bucketMs / 2 - ms)
    if (best === null || distance < Math.abs(points[best]!.ms + bucketMs / 2 - ms)) {
      best = index
    }
  }
  return best
}

export function formatInches(value: number): string {
  return value.toFixed(2)
}

export default function WeatherChart(props: WeatherChartProps) {
  const { points, fromMs, toMs, bucketMs, ticks, rainTotal, et0Total, hover, onHover } = props
  const [wrapper, width] = useWidth()

  const plotWidth = Math.max(width - MARGIN.left - MARGIN.right, 10)
  const plotHeight = HEIGHT - MARGIN.top - MARGIN.bottom

  const highest = Math.max(0, ...points.map((point) => Math.max(point.rain ?? 0, point.et0 ?? 0)))
  const step = waterStep(highest)
  const yHigh = Math.max(step, Math.ceil(highest / step - 1e-9) * step)

  const x = (ms: number) => MARGIN.left + ((Math.min(Math.max(ms, fromMs), toMs) - fromMs) / (toMs - fromMs)) * plotWidth
  const y = (value: number) => MARGIN.top + (1 - value / yHigh) * plotHeight

  const yTicks: number[] = []
  for (let value = 0; value <= yHigh + step / 1000; value += step) {
    yTicks.push(Number(value.toFixed(2)))
  }

  const bars = points
    .filter((point) => point.rain !== null && point.rain > 0)
    .map((point) => {
      const left = x(point.ms)
      const right = x(point.ms + bucketMs)
      const gap = right - left > 4 ? 1 : 0
      return { ms: point.ms, x: left + gap, width: Math.max(right - left - gap * 2, 1), y: y(point.rain!) }
    })

  const runs: WaterPoint[][] = []
  let run: WaterPoint[] = []
  for (const point of points) {
    const previous = run[run.length - 1]
    if (point.et0 === null || (previous !== undefined && point.ms - previous.ms > bucketMs * 1.5)) {
      if (run.length > 0) {
        runs.push(run)
      }
      run = []
    }
    if (point.et0 !== null) {
      run.push(point)
    }
  }
  if (run.length > 0) {
    runs.push(run)
  }
  const centre = (point: WaterPoint) => point.ms + bucketMs / 2
  const lines = runs.map(
    (segment) => `M${segment.map((point) => `${x(centre(point)).toFixed(1)},${y(point.et0!).toFixed(1)}`).join('L')}`,
  )

  const selected = hover === null ? undefined : points[hover]

  const onPointerMove = (event: PointerEvent<SVGRectElement>) => {
    const box = event.currentTarget.getBoundingClientRect()
    if (box.width <= 0 || points.length === 0) {
      return
    }
    onHover(nearestWaterIndex(points, bucketMs, fromMs + ((event.clientX - box.left) / box.width) * (toMs - fromMs)))
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
        Rain and ET₀ <span className="climate-chart__unit">in</span>
        <span className="climate-legend">
          <span className="climate-legend__item">
            <span className="climate-legend__bar" aria-hidden="true" />
            Rain {formatInches(rainTotal)}
          </span>
          <span className="climate-legend__item">
            <span className="climate-legend__line" style={{ borderTopColor: 'var(--et0)' }} aria-hidden="true" />
            ET₀ {formatInches(et0Total)}
          </span>
        </span>
      </figcaption>
      <div ref={wrapper} className="climate-chart__plot">
        <svg
          width={width}
          height={HEIGHT}
          viewBox={`0 0 ${width} ${HEIGHT}`}
          role="img"
          aria-label={`Rain and ET₀ from Open-Meteo: ${formatInches(rainTotal)} in of rain and ${formatInches(et0Total)} in of ET₀ in this range`}
          tabIndex={0}
          onKeyDown={onKeyDown}
          onBlur={() => {
            onHover(null)
          }}
        >
          {yTicks.map((value) => (
            <g key={value}>
              <line className="climate-chart__grid" x1={MARGIN.left} x2={MARGIN.left + plotWidth} y1={y(value)} y2={y(value)} />
              <text className="climate-chart__tick" x={MARGIN.left - 6} y={y(value)} textAnchor="end" dominantBaseline="middle">
                {value}
              </text>
            </g>
          ))}
          {ticks.map((tick) => (
            <text key={tick.ms} className="climate-chart__tick" x={x(tick.ms)} y={HEIGHT - 6} textAnchor="middle">
              {tick.label}
            </text>
          ))}
          {bars.map((bar) => (
            <rect
              key={bar.ms}
              x={bar.x}
              y={bar.y}
              width={bar.width}
              height={Math.max(MARGIN.top + plotHeight - bar.y, 0)}
              rx={Math.min(2, bar.width / 2)}
              fill="var(--rain)"
            />
          ))}
          {lines.map((path, index) => (
            <path
              key={`et0-${index}`}
              d={path}
              fill="none"
              stroke="var(--et0)"
              strokeWidth={2}
              strokeLinejoin="round"
              strokeLinecap="round"
            />
          ))}
          {selected === undefined ? null : (
            <g data-testid="weather-crosshair">
              <line
                className="climate-chart__crosshair"
                x1={x(centre(selected))}
                x2={x(centre(selected))}
                y1={MARGIN.top}
                y2={MARGIN.top + plotHeight}
              />
              {selected.et0 === null ? null : (
                <circle
                  cx={x(centre(selected))}
                  cy={y(selected.et0)}
                  r={4}
                  fill="var(--et0)"
                  stroke="var(--chart-surface)"
                  strokeWidth={2}
                />
              )}
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
