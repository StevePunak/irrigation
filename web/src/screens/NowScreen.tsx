import { useCallback, useEffect, useState } from 'react'
import { getZones, runZone, stopAll } from '../api/client'
import type { Zone } from '../api/types'
import StopButton from '../components/StopButton'
import ZoneTile from '../components/ZoneTile'
import { useCountdown } from '../hooks/useCountdown'
import { formatCountdown, formatDayAndClock, formatDuration } from '../time/zonedformat'
import type { ScreenProps } from './screenProps'

export const QUICK_RUN_CHOICES = [60, 300, 600, 900, 1200, 1800]
const DEFAULT_QUICK_RUN = 600

export default function NowScreen({ status, polls, refresh }: ScreenProps) {
  const [zones, setZones] = useState<Zone[]>([])
  const [seconds, setSeconds] = useState(DEFAULT_QUICK_RUN)
  const [error, setError] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)

  useEffect(() => {
    let cancelled = false
    getZones()
      .then((loaded) => {
        if (cancelled === false) {
          setZones([...loaded].sort((left, right) => left.number - right.number))
        }
      })
      .catch((caught: unknown) => {
        if (cancelled === false) {
          setError(caught instanceof Error ? caught.message : String(caught))
        }
      })
    return () => {
      cancelled = true
    }
  }, [])

  const runningZone = status?.runningZone ?? 0
  const remaining = useCountdown(status?.secondsRemaining ?? 0, polls)
  const running = zones.find((zone) => zone.number === runningZone)

  const onRun = useCallback(
    (zone: Zone) => {
      setError(null)
      setBusy(true)
      runZone(zone, seconds)
        .then(() => {
          refresh()
        })
        .catch((caught: unknown) => {
          setError(caught instanceof Error ? caught.message : String(caught))
        })
        .finally(() => {
          setBusy(false)
        })
    },
    [seconds, refresh],
  )

  const onStop = useCallback(() => {
    setError(null)
    setBusy(true)
    stopAll()
      .then(() => {
        refresh()
      })
      .catch((caught: unknown) => {
        setError(caught instanceof Error ? caught.message : String(caught))
      })
      .finally(() => {
        setBusy(false)
      })
  }, [refresh])

  const zone = status?.timezone ?? ''

  return (
    <section className="screen">
      <h1>Now</h1>

      {error === null ? null : (
        <div className="alert" role="alert">
          {error}
        </div>
      )}

      <div className="running" data-testid="running-banner">
        {runningZone > 0 ? (
          <>
            <span className="running__zone">
              Zone {runningZone} — {running?.name ?? ''}
            </span>
            <span className="running__clock">{formatCountdown(remaining)}</span>
          </>
        ) : (
          <span className="running__idle">No zone running</span>
        )}
      </div>

      <StopButton onStop={onStop} busy={busy} />

      <div className="next-run" data-testid="next-run">
        Next run:{' '}
        {status !== null && status.nextRunUtc !== null
          ? formatDayAndClock(status.nextRunUtc, zone)
          : 'none scheduled'}
      </div>

      <label className="quick-run">
        Run for
        <select
          value={seconds}
          onChange={(event) => {
            setSeconds(Number(event.target.value))
          }}
        >
          {QUICK_RUN_CHOICES.map((choice) => (
            <option key={choice} value={choice}>
              {formatDuration(choice)}
            </option>
          ))}
        </select>
      </label>

      <div className="zone-grid">
        {zones.map((entry) => (
          <ZoneTile
            key={entry.number}
            zone={entry}
            running={entry.number === runningZone}
            disabled={busy}
            onRun={onRun}
          />
        ))}
      </div>
    </section>
  )
}
