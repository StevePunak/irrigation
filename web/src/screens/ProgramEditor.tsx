import { useCallback, useState } from 'react'
import { createProgram, deleteProgram, updateProgram } from '../api/client'
import { DAY_MODES, type DayMode, type Program, type ProgramDraft, type Zone } from '../api/types'
import { WEEKDAY_LABELS, toDraft, toggleWeekday } from '../programs/dayRule'
import { inputValueToMinutes, minutesToInputValue } from '../time/zonedformat'

const DAY_MODE_LABELS: Record<DayMode, string> = {
  DaysOfWeek: 'Days of week',
  Odd: 'Odd days',
  Even: 'Even days',
  EveryNDays: 'Every N days',
}

export function emptyDraft(controllerZone: string): ProgramDraft {
  return {
    name: '',
    enabled: true,
    dayMode: 'DaysOfWeek',
    dowMask: 0,
    intervalDays: 0,
    anchorDate: null,
    startTimes: [{ minutesAfterMidnight: 360, timezone: controllerZone }],
    zones: [],
  }
}

export function validationError(draft: ProgramDraft): string | null {
  if (draft.name.trim().length === 0) {
    return 'Give the program a name.'
  }
  if (draft.startTimes.length === 0) {
    return 'Add at least one start time.'
  }
  if (draft.zones.length === 0) {
    return 'Add at least one zone.'
  }
  if (draft.zones.some((zone) => zone.durationSeconds < 1)) {
    return 'Every zone needs a duration of at least one minute.'
  }
  if (draft.dayMode === 'DaysOfWeek' && (draft.dowMask & 0b1111111) === 0) {
    return 'Select at least one day of the week.'
  }
  if (draft.dayMode === 'EveryNDays') {
    if (draft.intervalDays < 1) {
      return 'Set an interval of at least one day.'
    }
    if (draft.anchorDate === null || draft.anchorDate.length === 0) {
      return 'Set a start date for the interval.'
    }
  }
  return null
}

/** Rewrites `sequence` from array position so a reorder reaches the daemon. */
function resequence(zones: ProgramDraft['zones']): ProgramDraft['zones'] {
  return zones.map((zone, index) => ({ ...zone, sequence: index + 1 }))
}

export interface ProgramEditorProps {
  program: Program | null
  zones: Zone[]
  controllerZone: string
  onDone: () => void
  onCancel: () => void
}

export default function ProgramEditor({
  program,
  zones,
  controllerZone,
  onDone,
  onCancel,
}: ProgramEditorProps) {
  const [draft, setDraft] = useState<ProgramDraft>(() =>
    program === null ? emptyDraft(controllerZone) : toDraft(program),
  )
  const [error, setError] = useState<string | null>(null)
  const [saving, setSaving] = useState(false)

  const patch = useCallback((changes: Partial<ProgramDraft>) => {
    setDraft((current) => ({ ...current, ...changes }))
  }, [])

  const onSave = useCallback(async () => {
    const normalised: ProgramDraft = { ...draft, name: draft.name.trim(), zones: resequence(draft.zones) }
    const invalid = validationError(normalised)
    if (invalid !== null) {
      setError(invalid)
      return
    }

    setSaving(true)
    try {
      if (program === null) {
        await createProgram(normalised)
      } else {
        await updateProgram(program.id, normalised)
      }
      onDone()
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught))
    } finally {
      setSaving(false)
    }
  }, [draft, program, onDone])

  const onDelete = useCallback(async () => {
    if (program === null) {
      return
    }
    if (globalThis.confirm(`Delete "${program.name}"? This cannot be undone.`) === false) {
      return
    }
    try {
      await deleteProgram(program.id)
      onDone()
    } catch (caught: unknown) {
      setError(caught instanceof Error ? caught.message : String(caught))
    }
  }, [program, onDone])

  const moveZone = useCallback((index: number, delta: number) => {
    setDraft((current) => {
      const target = index + delta
      if (target < 0 || target >= current.zones.length) {
        return current
      }
      const reordered = [...current.zones]
      const [moved] = reordered.splice(index, 1)
      reordered.splice(target, 0, moved!)
      return { ...current, zones: resequence(reordered) }
    })
  }, [])

  return (
    <section className="screen editor">
      <h2>{program === null ? 'New program' : `Edit ${program.name}`}</h2>

      {error === null ? null : (
        <div className="alert" role="alert">
          {error}
        </div>
      )}

      <label>
        Program name
        <input
          type="text"
          value={draft.name}
          onChange={(event) => {
            patch({ name: event.target.value })
          }}
        />
      </label>

      <label>
        Enabled
        <input
          type="checkbox"
          checked={draft.enabled}
          onChange={(event) => {
            patch({ enabled: event.target.checked })
          }}
        />
      </label>

      <label>
        Day rule
        <select
          value={draft.dayMode}
          onChange={(event) => {
            patch({ dayMode: event.target.value as DayMode })
          }}
        >
          {DAY_MODES.map((mode) => (
            <option key={mode} value={mode}>
              {DAY_MODE_LABELS[mode]}
            </option>
          ))}
        </select>
      </label>

      {draft.dayMode === 'DaysOfWeek' ? (
        <div className="weekdays">
          {WEEKDAY_LABELS.map((label, index) => (
            <button
              key={label}
              type="button"
              aria-pressed={(draft.dowMask & (1 << index)) !== 0}
              onClick={() => {
                patch({ dowMask: toggleWeekday(draft.dowMask, index) })
              }}
            >
              {label}
            </button>
          ))}
        </div>
      ) : null}

      {draft.dayMode === 'EveryNDays' ? (
        <>
          <label>
            Interval in days
            <input
              type="number"
              min={1}
              value={draft.intervalDays}
              onChange={(event) => {
                patch({ intervalDays: Number(event.target.value) })
              }}
            />
          </label>
          <label>
            Starting on
            <input
              type="date"
              value={draft.anchorDate ?? ''}
              onChange={(event) => {
                patch({ anchorDate: event.target.value === '' ? null : event.target.value })
              }}
            />
          </label>
        </>
      ) : null}

      <h3>Start times</h3>
      {draft.startTimes.map((start, index) => (
        <div key={index} className="row">
          <label>
            {`Start time ${index + 1}`}
            <input
              type="time"
              defaultValue={minutesToInputValue(start.minutesAfterMidnight)}
              onChange={(event) => {
                const minutes = inputValueToMinutes(event.target.value)
                if (minutes < 0) {
                  return
                }
                patch({
                  startTimes: draft.startTimes.map((entry, i) =>
                    i === index ? { ...entry, minutesAfterMidnight: minutes } : entry,
                  ),
                })
              }}
            />
          </label>
          <span className="row__zone">{start.timezone}</span>
          <button
            type="button"
            onClick={() => {
              patch({ startTimes: draft.startTimes.filter((_, i) => i !== index) })
            }}
          >
            {`Remove start time ${index + 1}`}
          </button>
        </div>
      ))}
      <button
        type="button"
        onClick={() => {
          patch({
            startTimes: [...draft.startTimes, { minutesAfterMidnight: 360, timezone: controllerZone }],
          })
        }}
      >
        Add start time
      </button>

      <h3>Zones in run order</h3>
      {draft.zones.map((zone, index) => (
        <div key={index} className="row">
          <label>
            {`Zone ${index + 1} valve`}
            <select
              value={String(zone.zoneId)}
              onChange={(event) => {
                patch({
                  zones: draft.zones.map((entry, i) =>
                    i === index ? { ...entry, zoneId: Number(event.target.value) } : entry,
                  ),
                })
              }}
            >
              {zones.map((candidate) => (
                <option key={candidate.id} value={String(candidate.id)}>
                  {`${candidate.number} · ${candidate.name}`}
                </option>
              ))}
            </select>
          </label>
          <label>
            {`Zone ${index + 1} minutes`}
            <input
              type="number"
              min={1}
              value={Math.round(zone.durationSeconds / 60)}
              onChange={(event) => {
                patch({
                  zones: draft.zones.map((entry, i) =>
                    i === index ? { ...entry, durationSeconds: Number(event.target.value) * 60 } : entry,
                  ),
                })
              }}
            />
          </label>
          <button
            type="button"
            onClick={() => {
              moveZone(index, -1)
            }}
          >
            {`Move zone ${index + 1} up`}
          </button>
          <button
            type="button"
            onClick={() => {
              moveZone(index, 1)
            }}
          >
            {`Move zone ${index + 1} down`}
          </button>
          <button
            type="button"
            onClick={() => {
              patch({ zones: resequence(draft.zones.filter((_, i) => i !== index)) })
            }}
          >
            {`Remove zone ${index + 1}`}
          </button>
        </div>
      ))}
      <button
        type="button"
        onClick={() => {
          const first = zones[0]
          if (first === undefined) {
            return
          }
          patch({
            zones: resequence([
              ...draft.zones,
              { zoneId: first.id, sequence: draft.zones.length + 1, durationSeconds: 600 },
            ]),
          })
        }}
      >
        Add zone
      </button>

      <div className="row">
        <button
          type="button"
          disabled={saving}
          onClick={() => {
            void onSave()
          }}
        >
          Save
        </button>
        <button type="button" onClick={onCancel}>
          Cancel
        </button>
        {program === null ? null : (
          <button
            type="button"
            className="destructive"
            onClick={() => {
              void onDelete()
            }}
          >
            Delete
          </button>
        )}
      </div>
    </section>
  )
}
