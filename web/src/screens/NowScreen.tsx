import { useCallback, useEffect, useRef, useState } from 'react'
import { getZones, runZone, stopAll, stopZone } from '../api/client'
import type { RunningProgram, RunningZone, Zone } from '../api/types'
import StopButton from '../components/StopButton'
import ZoneTile from '../components/ZoneTile'
import { useCountdown } from '../hooks/useCountdown'
import { formatCountdown, formatDayAndClock, formatDuration } from '../time/zonedformat'
import type { ScreenProps } from './screenProps'

export const QUICK_RUN_CHOICES = [60, 300, 600, 900, 1200, 1800]
const DEFAULT_QUICK_RUN = 600

/** Reads "Morning Drip — step 1 of 2, zone 7 waiting". */
export function programLine(program: RunningProgram): string {
  const base = `${program.name} — step ${program.step} of ${program.stepCount}`
  const waiting = program.waitingZones
  if (waiting.length === 0) {
    return base
  }
  return `${base}, ${waiting.length === 1 ? 'zone' : 'zones'} ${waiting.join(', ')} waiting`
}

interface RunningRowProps {
  entry: RunningZone
  name: string
  polls: number
  onStop: (zoneNumber: number) => void
}

/** The row's Stop carries no disabled state: whoever is standing in the spray must be able to close this valve whatever else is in flight. */
function RunningRow({ entry, name, polls, onStop }: RunningRowProps) {
  const remaining = useCountdown(entry.secondsRemaining, polls)
  return (
    <li className="running-row" data-testid={`running-zone-${entry.zone}`}>
      <span className="running-row__zone">
        Zone {entry.zone} — {name}
      </span>
      <span className={`running-row__tag running-row__tag--${entry.source}`}>
        {entry.source === 'program' ? 'Program' : 'Manual'}
      </span>
      <span className="running__clock">{formatCountdown(remaining)}</span>
      <button
        type="button"
        className="running-row__stop"
        aria-label={`Stop zone ${entry.zone}`}
        onClick={() => {
          onStop(entry.zone)
        }}
      >
        Stop
      </button>
    </li>
  )
}

export default function NowScreen({ status, polls, refresh }: ScreenProps) {
  const [zones, setZones] = useState<Zone[]>([])
  const [seconds, setSeconds] = useState(DEFAULT_QUICK_RUN)
  const [error, setError] = useState<string | null>(null)
  const [busy, setBusy] = useState(false)
  const actionSeq = useRef(0)
  const inFlight = useRef(0)

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

  const act = useCallback(
    (action: () => Promise<void>) => {
      const seq = (actionSeq.current += 1)
      inFlight.current += 1
      setError(null)
      setBusy(true)
      action()
        .then(() => {
          refresh()
        })
        .catch((caught: unknown) => {
          if (actionSeq.current === seq) {
            setError(caught instanceof Error ? caught.message : String(caught))
          }
        })
        .finally(() => {
          inFlight.current -= 1
          setBusy(inFlight.current > 0)
        })
    },
    [refresh],
  )

  const onRun = useCallback(
    (zone: Zone) => {
      act(() => runZone(zone, seconds))
    },
    [act, seconds],
  )

  const onStop = useCallback(() => {
    act(stopAll)
  }, [act])

  const onStopZone = useCallback(
    (zoneNumber: number) => {
      act(() => stopZone(zoneNumber))
    },
    [act],
  )

  const running = status?.running ?? []
  const cap = status?.maxConcurrentZones ?? 0
  const atCap = status !== null && running.length >= cap
  const nameOf = (zoneNumber: number) => zones.find((zone) => zone.number === zoneNumber)?.name ?? ''
  const zoneId = status?.timezone ?? ''

  return (
    <section className="screen">
      <h1>Now</h1>

      {error === null ? null : (
        <div className="alert" role="alert">
          {error}
        </div>
      )}

      <div className="running" data-testid="running-banner">
        {status === null ? (
          <span className="running__unknown">Zone state unknown</span>
        ) : running.length === 0 ? (
          <span className="running__idle">No zone running</span>
        ) : (
          <ul className="running-list">
            {running.map((entry) => (
              <RunningRow
                key={entry.zone}
                entry={entry}
                name={nameOf(entry.zone)}
                polls={polls}
                onStop={onStopZone}
              />
            ))}
          </ul>
        )}
        {status !== null && status.program !== null ? (
          <div className="program-line" data-testid="program-line">
            {programLine(status.program)}
          </div>
        ) : null}
        {status !== null && status.queue.length > 0 ? (
          <div className="queue-line" data-testid="queue-line">
            Queued: {status.queue.map((entry) => entry.name).join(', ')}
          </div>
        ) : null}
      </div>

      <StopButton onStop={onStop} busy={busy} />

      <div className="next-run" data-testid="next-run">
        Next run:{' '}
        {status === null
          ? 'unknown'
          : status.nextRunUtc !== null
            ? formatDayAndClock(status.nextRunUtc, zoneId)
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

      {atCap ? (
        <p className="cap-hint" data-testid="cap-hint">
          {`${running.length} of ${cap} running`}
        </p>
      ) : null}

      <div className="zone-grid">
        {zones.map((entry) => {
          const open = running.some((item) => item.zone === entry.number)
          return (
            <ZoneTile
              key={entry.number}
              zone={entry}
              running={open}
              disabled={busy || (atCap && open === false)}
              onRun={onRun}
              onStop={onStopZone}
            />
          )
        })}
      </div>
    </section>
  )
}
