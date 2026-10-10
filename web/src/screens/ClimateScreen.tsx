import { useEffect, useRef, useState } from 'react'
import { getClimate } from '../api/client'
import { ApiError, type ClimateBucket, type ClimateHistory } from '../api/types'
import ClimateChart, { type AxisTick, type ChartPoint } from '../climate/ClimateChart'
import { axisTicks, formatAxisTick, formatDayAndClock } from '../time/zonedformat'
import type { ScreenProps } from './screenProps'

export const CLIMATE_RANGES = [
  { hours: 24, label: '24 h' },
  { hours: 168, label: '7 days' },
  { hours: 720, label: '30 days' },
  { hours: 8760, label: '1 year' },
]
const REFRESH_MS = 60000

export function toFahrenheit(celsius: number): number {
  return (celsius * 9) / 5 + 32
}

function temperaturePoint(bucket: ClimateBucket): ChartPoint {
  return {
    ms: Date.parse(bucket.startUtc),
    min: toFahrenheit(bucket.temperatureC.min),
    mean: toFahrenheit(bucket.temperatureC.mean),
    max: toFahrenheit(bucket.temperatureC.max),
  }
}

function humidityPoint(bucket: ClimateBucket): ChartPoint {
  return { ms: Date.parse(bucket.startUtc), ...bucket.humidityPercent }
}

/** Reads "74°F (71–77)" in whole units: the bucket mean, then its extremes. */
export function rangeText(min: number, mean: number, max: number, unit: string): string {
  const whole = (value: number) => Math.round(value)
  if (whole(min) === whole(max)) {
    return `${whole(mean)}${unit}`
  }
  return `${whole(mean)}${unit} (${whole(min)}–${whole(max)})`
}

export default function ClimateScreen({ status }: ScreenProps) {
  const [hours, setHours] = useState(24)
  const [history, setHistory] = useState<ClimateHistory | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)
  const [hover, setHover] = useState<number | null>(null)
  const request = useRef(0)

  useEffect(() => {
    let cancelled = false
    const load = () => {
      const seq = (request.current += 1)
      setLoading(true)
      getClimate(hours)
        .then((loaded) => {
          if (cancelled === false && request.current === seq) {
            setHistory(loaded)
            setError(null)
          }
        })
        .catch((caught: unknown) => {
          if (cancelled === false && request.current === seq) {
            setError(
              caught instanceof ApiError && caught.status === 404
                ? 'No climate sensor is configured.'
                : caught instanceof Error
                  ? caught.message
                  : String(caught),
            )
          }
        })
        .finally(() => {
          if (cancelled === false && request.current === seq) {
            setLoading(false)
          }
        })
    }
    load()
    const timer = window.setInterval(load, REFRESH_MS)
    return () => {
      cancelled = true
      window.clearInterval(timer)
    }
  }, [hours])

  const zone = status?.timezone ?? Intl.DateTimeFormat().resolvedOptions().timeZone
  const buckets = history?.buckets ?? []
  const fromMs = history === null ? 0 : Date.parse(history.fromUtc)
  const toMs = history === null ? 0 : Date.parse(history.toUtc)
  const bucketMs = (history?.bucketSeconds ?? 0) * 1000
  const span = history === null ? hours : Math.round((toMs - fromMs) / 3600000)
  const ticks: AxisTick[] =
    history === null
      ? []
      : axisTicks(fromMs, toMs, zone, span).map((ms) => ({ ms, label: formatAxisTick(ms, zone, span) }))
  const temperature = buckets.map(temperaturePoint)
  const humidity = buckets.map(humidityPoint)
  const shown = hover === null ? buckets[buckets.length - 1] : buckets[hover]
  const index = hover ?? buckets.length - 1

  return (
    <section className="screen">
      <h1>Climate</h1>

      <div className="climate-ranges" role="group" aria-label="Range">
        {CLIMATE_RANGES.map((range) => (
          <button
            key={range.hours}
            type="button"
            aria-pressed={hours === range.hours}
            onClick={() => {
              setHover(null)
              setHours(range.hours)
            }}
          >
            {range.label}
          </button>
        ))}
      </div>

      {error === null ? null : (
        <div className="alert" role="alert">
          {error}
        </div>
      )}

      {history !== null && buckets.length === 0 ? <p className="climate-empty">No readings in this range yet.</p> : null}

      {shown === undefined ? null : (
        <div className="climate-readout" data-testid="climate-readout" aria-live="polite">
          <span className="climate-readout__when">
            {hover === null ? 'Latest' : formatDayAndClock(shown.startUtc, zone)}
          </span>
          <span className="climate-readout__value">
            <span className="climate-readout__key climate-readout__key--temperature" aria-hidden="true" />
            {rangeText(
              toFahrenheit(shown.temperatureC.min),
              toFahrenheit(shown.temperatureC.mean),
              toFahrenheit(shown.temperatureC.max),
              '°F',
            )}
          </span>
          <span className="climate-readout__value">
            <span className="climate-readout__key climate-readout__key--humidity" aria-hidden="true" />
            {rangeText(shown.humidityPercent.min, shown.humidityPercent.mean, shown.humidityPercent.max, '%')}
          </span>
        </div>
      )}

      {buckets.length === 0 ? null : (
        <div className={`climate-charts${loading ? ' climate-charts--loading' : ''}`}>
          <ClimateChart
            title="Temperature"
            unit="°F"
            points={temperature}
            fromMs={fromMs}
            toMs={toMs}
            bucketMs={bucketMs}
            ticks={ticks}
            step={5}
            color="var(--temperature)"
            hover={hover === null ? null : index}
            onHover={setHover}
          />
          <ClimateChart
            title="Humidity"
            unit="% RH"
            points={humidity}
            fromMs={fromMs}
            toMs={toMs}
            bucketMs={bucketMs}
            ticks={ticks}
            step={10}
            floor={0}
            ceiling={100}
            color="var(--humidity)"
            hover={hover === null ? null : index}
            onHover={setHover}
          />
        </div>
      )}

      {buckets.length === 0 ? null : (
        <details className="climate-table">
          <summary>Show as table</summary>
          <div className="climate-table__scroll">
            <table>
              <thead>
                <tr>
                  <th scope="col">From</th>
                  <th scope="col">°F mean</th>
                  <th scope="col">°F range</th>
                  <th scope="col">% RH mean</th>
                  <th scope="col">% RH range</th>
                </tr>
              </thead>
              <tbody>
                {[...buckets].reverse().map((bucket) => (
                  <tr key={bucket.startUtc}>
                    <td>{formatDayAndClock(bucket.startUtc, zone)}</td>
                    <td>{Math.round(toFahrenheit(bucket.temperatureC.mean))}</td>
                    <td>
                      {Math.round(toFahrenheit(bucket.temperatureC.min))}–{Math.round(toFahrenheit(bucket.temperatureC.max))}
                    </td>
                    <td>{Math.round(bucket.humidityPercent.mean)}</td>
                    <td>
                      {Math.round(bucket.humidityPercent.min)}–{Math.round(bucket.humidityPercent.max)}
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </details>
      )}
    </section>
  )
}
