import { useEffect, useRef, useState } from 'react'
import { getClimate } from '../api/client'
import { ApiError, type ClimateBucket, type ClimateHistory, type WeatherBucket } from '../api/types'
import ClimateChart, { nearestIndex, type AxisTick, type ChartPoint } from '../climate/ClimateChart'
import WeatherChart, { formatInches, nearestWaterIndex, type WaterPoint } from '../climate/WeatherChart'
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

const HOUR_MS = 3600000
const MM_PER_INCH = 25.4

export function toInches(millimetres: number): number {
  return millimetres / MM_PER_INCH
}

/**
 * Open-Meteo's temperature and humidity are hourly instants, the first at the bucket start;
 * their mean sits midway between the first and last instant.
 */
function referencePoints(
  buckets: WeatherBucket[],
  bucketMs: number,
  pick: (bucket: WeatherBucket) => number | null,
): ChartPoint[] {
  const offset = Math.max(bucketMs - HOUR_MS, 0) / 2
  const points: ChartPoint[] = []
  for (const bucket of buckets) {
    const value = pick(bucket)
    if (value !== null) {
      points.push({ ms: Date.parse(bucket.startUtc) + offset, min: value, mean: value, max: value })
    }
  }
  return points
}

function waterPoint(bucket: WeatherBucket): WaterPoint {
  return {
    ms: Date.parse(bucket.startUtc),
    rain: bucket.precipitationMm === null ? null : toInches(bucket.precipitationMm),
    et0: bucket.et0Mm === null ? null : toInches(bucket.et0Mm),
  }
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
  const [hoverMs, setHoverMs] = useState<number | null>(null)
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
  const weatherBuckets = history?.weather.buckets ?? []
  const weatherBucketMs = (history?.weather.bucketSeconds ?? 0) * 1000
  const water = weatherBuckets.map(waterPoint)
  const rainTotal = water.reduce((sum, point) => sum + (point.rain ?? 0), 0)
  const et0Total = water.reduce((sum, point) => sum + (point.et0 ?? 0), 0)
  const spacingMs = Math.max(weatherBucketMs, HOUR_MS)
  const temperatureReference = referencePoints(weatherBuckets, weatherBucketMs, (bucket) =>
    bucket.temperatureC === null ? null : toFahrenheit(bucket.temperatureC),
  )
  const humidityReference = referencePoints(weatherBuckets, weatherBucketMs, (bucket) => bucket.humidityPercent)

  const sensorIndex = hoverMs === null || temperature.length === 0 ? null : nearestIndex(temperature, bucketMs, hoverMs)
  const weatherIndex = hoverMs === null ? null : nearestWaterIndex(water, weatherBucketMs, hoverMs)
  const shown = buckets[sensorIndex ?? buckets.length - 1]
  const latest = (pick: (bucket: WeatherBucket) => number | null) =>
    [...weatherBuckets].reverse().find((bucket) => pick(bucket) !== null)
  const shownWeather: WeatherBucket | undefined =
    weatherIndex !== null
      ? weatherBuckets[weatherIndex]
      : weatherBuckets.length === 0
        ? undefined
        : {
            startUtc: weatherBuckets[weatherBuckets.length - 1]!.startUtc,
            temperatureC: latest((bucket) => bucket.temperatureC)?.temperatureC ?? null,
            humidityPercent: latest((bucket) => bucket.humidityPercent)?.humidityPercent ?? null,
            precipitationMm: latest((bucket) => bucket.precipitationMm)?.precipitationMm ?? null,
            et0Mm: latest((bucket) => bucket.et0Mm)?.et0Mm ?? null,
          }
  const onSensorHover = (index: number | null) => {
    setHoverMs(index === null ? null : temperature[index]!.ms + bucketMs / 2)
  }
  const onWeatherHover = (index: number | null) => {
    setHoverMs(index === null ? null : water[index]!.ms + weatherBucketMs / 2)
  }

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
              setHoverMs(null)
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

      {shown === undefined && shownWeather === undefined ? null : (
        <div className="climate-readout" data-testid="climate-readout" aria-live="polite">
          <span className="climate-readout__when">
            {hoverMs === null
              ? 'Latest'
              : formatDayAndClock((shown ?? shownWeather)!.startUtc, zone)}
          </span>
          {shown === undefined ? null : (
            <>
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
            </>
          )}
          {shownWeather === undefined ? null : (
            <span className="climate-readout__weather" data-testid="climate-readout-weather">
              {`Open-Meteo ${
                shownWeather.temperatureC === null ? '–' : `${Math.round(toFahrenheit(shownWeather.temperatureC))}°F`
              } · ${shownWeather.humidityPercent === null ? '–' : `${Math.round(shownWeather.humidityPercent)}%`}`}
              {' · '}
              {`rain ${shownWeather.precipitationMm === null ? '–' : `${formatInches(toInches(shownWeather.precipitationMm))} in`}`}
              {' · '}
              {`ET₀ ${shownWeather.et0Mm === null ? '–' : `${formatInches(toInches(shownWeather.et0Mm))} in`}`}
            </span>
          )}
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
            seriesLabel="Sensor"
            reference={
              temperatureReference.length === 0
                ? undefined
                : { label: 'Open-Meteo', points: temperatureReference, spacingMs }
            }
            hover={sensorIndex}
            onHover={onSensorHover}
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
            seriesLabel="Sensor"
            reference={
              humidityReference.length === 0 ? undefined : { label: 'Open-Meteo', points: humidityReference, spacingMs }
            }
            hover={sensorIndex}
            onHover={onSensorHover}
          />
        </div>
      )}

      {history === null ? null : water.length === 0 ? (
        <p className="climate-empty" data-testid="weather-empty">
          No Open-Meteo weather in this range. Set the yard&apos;s location on the Settings page to start collecting it.
        </p>
      ) : (
        <div className={`climate-charts${loading ? ' climate-charts--loading' : ''}`}>
          <WeatherChart
            points={water}
            fromMs={fromMs}
            toMs={toMs}
            bucketMs={weatherBucketMs}
            ticks={ticks}
            rainTotal={rainTotal}
            et0Total={et0Total}
            hover={weatherIndex}
            onHover={onWeatherHover}
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

      {water.length === 0 ? null : (
        <details className="climate-table">
          <summary>Show Open-Meteo weather as table</summary>
          <div className="climate-table__scroll">
            <table>
              <thead>
                <tr>
                  <th scope="col">From</th>
                  <th scope="col">Rain in</th>
                  <th scope="col">ET₀ in</th>
                  <th scope="col">°F</th>
                  <th scope="col">% RH</th>
                </tr>
              </thead>
              <tbody>
                {[...weatherBuckets].reverse().map((bucket) => (
                  <tr key={bucket.startUtc}>
                    <td>{formatDayAndClock(bucket.startUtc, zone)}</td>
                    <td>{bucket.precipitationMm === null ? '–' : formatInches(toInches(bucket.precipitationMm))}</td>
                    <td>{bucket.et0Mm === null ? '–' : formatInches(toInches(bucket.et0Mm))}</td>
                    <td>{bucket.temperatureC === null ? '–' : Math.round(toFahrenheit(bucket.temperatureC))}</td>
                    <td>{bucket.humidityPercent === null ? '–' : Math.round(bucket.humidityPercent)}</td>
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
