import { useCallback, useEffect, useState } from 'react'
import { getPrograms, getZones, runProgram, updateProgram } from '../api/client'
import type { Program, Zone } from '../api/types'
import { dayRuleSummary, toDraft, totalRuntimeSeconds } from '../programs/dayRule'
import { formatDayAndClock, formatDuration, minutesToClock } from '../time/zonedformat'
import type { ScreenProps } from './screenProps'

export default function ProgramsScreen({ status }: ScreenProps) {
  const [programs, setPrograms] = useState<Program[] | null>(null)
  const [zones, setZones] = useState<Zone[]>([])
  const [error, setError] = useState<string | null>(null)

  const load = useCallback(async () => {
    try {
      const [loadedPrograms, loadedZones] = await Promise.all([getPrograms(), getZones()])
      setPrograms(loadedPrograms)
      setZones(loadedZones)
      setError(null)
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught))
    }
  }, [])

  useEffect(() => {
    void load()
  }, [load])

  const zoneName = useCallback(
    (zoneId: number) => zones.find((zone) => zone.id === zoneId)?.name ?? `Zone id ${zoneId}`,
    [zones],
  )

  const onToggle = useCallback(
    async (program: Program) => {
      let toggleError: string | null = null
      try {
        await updateProgram(program.id, { ...toDraft(program), enabled: program.enabled === false })
      } catch (caught: unknown) {
        toggleError = caught instanceof Error ? caught.message : String(caught)
      }
      await load()
      if (toggleError !== null) {
        setError(toggleError)
      }
    },
    [load],
  )

  const onRun = useCallback(async (program: Program) => {
    try {
      await runProgram(program.id)
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught))
    }
  }, [])

  const controllerZone = status?.timezone ?? ''

  return (
    <section className="screen">
      <h1>Programs</h1>

      {error === null ? null : (
        <div className="alert" role="alert">
          {error}
        </div>
      )}

      {programs !== null && programs.length === 0 ? <p>No programs yet.</p> : null}

      {(programs ?? []).map((program) => (
        <article key={program.id} className="program" data-testid={`program-${program.id}`}>
          <header className="program__header">
            <h2>{program.name}</h2>
            <button
              type="button"
              role="switch"
              aria-checked={program.enabled}
              aria-label={`${program.name} enabled`}
              onClick={() => {
                void onToggle(program)
              }}
            >
              {program.enabled ? 'Enabled' : 'Disabled'}
            </button>
          </header>

          <div data-testid="day-rule">{dayRuleSummary(program)}</div>

          <ul data-testid="start-times">
            {program.startTimes.map((start) => (
              <li key={start.id}>
                {minutesToClock(start.minutesAfterMidnight)}
                {start.timezone === controllerZone ? '' : ` (${start.timezone})`}
              </li>
            ))}
          </ul>

          <ol data-testid="zone-sequence">
            {program.zones.map((zone, index) => (
              <li key={zone.id}>
                {index + 1}. {zoneName(zone.zoneId)} {formatDuration(zone.durationSeconds)}
              </li>
            ))}
          </ol>

          <div data-testid="total-runtime">
            Total {formatDuration(totalRuntimeSeconds(program.zones))}
          </div>

          <div data-testid="next-run">
            Next run:{' '}
            {program.nextRunUtc === null ? '—' : formatDayAndClock(program.nextRunUtc, controllerZone)}
          </div>

          <button
            type="button"
            onClick={() => {
              void onRun(program)
            }}
          >
            Run now
          </button>
        </article>
      ))}
    </section>
  )
}
