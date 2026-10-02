import { useCallback, useState } from 'react'
import { createProgram, deleteProgram, updateProgram } from '../api/client'
import { DAY_MODES, type DayMode, type Program, type ProgramDraft, type ProgramStep, type Zone } from '../api/types'
import { WEEKDAY_LABELS, toDraft, toggleWeekday, totalRuntimeSeconds } from '../programs/dayRule'
import { formatDuration, inputValueToMinutes, minutesToInputValue } from '../time/zonedformat'

const DAY_MODE_LABELS: Record<DayMode, string> = {
  DaysOfWeek: 'Days of week',
  Odd: 'Odd days',
  Even: 'Even days',
  EveryNDays: 'Every N days',
}

const DEFAULT_STEP_SECONDS = 600

export function emptyDraft(controllerZone: string): ProgramDraft {
  return {
    name: '',
    enabled: true,
    dayMode: 'DaysOfWeek',
    dowMask: 0,
    intervalDays: 0,
    anchorDate: null,
    startTimes: [{ minutesAfterMidnight: 360, timezone: controllerZone }],
    steps: [],
  }
}

export function validationError(draft: ProgramDraft): string | null {
  if (draft.name.trim().length === 0) {
    return 'Give the program a name.'
  }
  if (draft.startTimes.length === 0) {
    return 'Add at least one start time.'
  }
  if (draft.startTimes.some((start) => start.timezone.length === 0)) {
    return 'The controller timezone is not known yet. Save again once the controller answers.'
  }
  if (draft.steps.length === 0) {
    return 'Add at least one step.'
  }
  const empty = draft.steps.findIndex((step) => step.zones.length === 0)
  if (empty >= 0) {
    return `Step ${empty + 1} needs at least one zone.`
  }
  if (draft.steps.some((step) => step.durationSeconds < 1)) {
    return 'Every step needs a duration of at least one minute.'
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

export interface ProgramEditorProps {
  program: Program | null
  zones: Zone[]
  controllerZone: string
  maxConcurrentZones: number
  onDone: () => void
  onCancel: () => void
}

export default function ProgramEditor({
  program,
  zones,
  controllerZone,
  maxConcurrentZones,
  onDone,
  onCancel,
}: ProgramEditorProps) {
  const [draft, setDraft] = useState<ProgramDraft>(() =>
    program === null ? emptyDraft(controllerZone) : toDraft(program),
  )
  const [startTimeText, setStartTimeText] = useState<string[]>(() =>
    (program === null ? emptyDraft(controllerZone) : toDraft(program)).startTimes.map((start) =>
      minutesToInputValue(start.minutesAfterMidnight),
    ),
  )
  const [error, setError] = useState<string | null>(null)
  const [saving, setSaving] = useState(false)

  const patch = useCallback((changes: Partial<ProgramDraft>) => {
    setDraft((current) => ({ ...current, ...changes }))
  }, [])

  const patchStep = useCallback((index: number, change: (step: ProgramStep) => ProgramStep) => {
    setDraft((current) => ({
      ...current,
      steps: current.steps.map((step, i) => (i === index ? change(step) : step)),
    }))
  }, [])

  const onSave = useCallback(async () => {
    const unparsed = startTimeText.findIndex((text) => inputValueToMinutes(text) < 0)
    if (unparsed >= 0) {
      setError(`Start time ${unparsed + 1} needs a valid time.`)
      return
    }
    const normalised: ProgramDraft = {
      ...draft,
      name: draft.name.trim(),
      startTimes: draft.startTimes.map((start) =>
        start.timezone.length === 0 ? { ...start, timezone: controllerZone } : start,
      ),
    }
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
  }, [draft, startTimeText, program, controllerZone, onDone])

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

  const moveStep = useCallback((index: number, delta: number) => {
    setDraft((current) => {
      const target = index + delta
      if (target < 0 || target >= current.steps.length) {
        return current
      }
      const reordered = [...current.steps]
      const [moved] = reordered.splice(index, 1)
      reordered.splice(target, 0, moved!)
      return { ...current, steps: reordered }
    })
  }, [])

  const zoneLabel = (zoneId: number) => {
    const zone = zones.find((candidate) => candidate.id === zoneId)
    return zone === undefined ? `Zone id ${zoneId}` : `${zone.number} · ${zone.name}`
  }

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
              value={startTimeText[index] ?? minutesToInputValue(start.minutesAfterMidnight)}
              onChange={(event) => {
                const text = event.target.value
                setStartTimeText((current) => current.map((entry, i) => (i === index ? text : entry)))
                const minutes = inputValueToMinutes(text)
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
              setStartTimeText((current) => current.filter((_, i) => i !== index))
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
          setStartTimeText((current) => [...current, minutesToInputValue(360)])
          patch({
            startTimes: [...draft.startTimes, { minutesAfterMidnight: 360, timezone: controllerZone }],
          })
        }}
      >
        Add start time
      </button>

      <h3>Steps in run order</h3>
      {draft.steps.map((step, index) => {
        const number = index + 1
        const available = zones.filter((zone) => step.zones.includes(zone.id) === false)
        return (
          <div key={index} className="step" data-testid={`step-${number}`}>
            <div className="row">
              <span className="step__title">{`Step ${number}`}</span>
              {step.zones.map((zoneId) => (
                <span key={zoneId} className="chip">
                  {zoneLabel(zoneId)}
                  <button
                    type="button"
                    className="chip__remove"
                    aria-label={`Remove ${zoneLabel(zoneId)} from step ${number}`}
                    onClick={() => {
                      patchStep(index, (current) => ({
                        ...current,
                        zones: current.zones.filter((id) => id !== zoneId),
                      }))
                    }}
                  >
                    ×
                  </button>
                </span>
              ))}
              <select
                aria-label={`Add a zone to step ${number}`}
                value=""
                onChange={(event) => {
                  if (event.target.value === '') {
                    return
                  }
                  const zoneId = Number(event.target.value)
                  patchStep(index, (current) => ({ ...current, zones: [...current.zones, zoneId] }))
                }}
              >
                <option value="">+ zone</option>
                {available.map((candidate) => (
                  <option key={candidate.id} value={String(candidate.id)}>
                    {`${candidate.number} · ${candidate.name}`}
                  </option>
                ))}
              </select>
            </div>
            <div className="row">
              <label>
                {`Step ${number} minutes`}
                <input
                  type="number"
                  min={1}
                  value={Math.round(step.durationSeconds / 60)}
                  onChange={(event) => {
                    const minutes = Number(event.target.value)
                    patchStep(index, (current) => ({ ...current, durationSeconds: minutes * 60 }))
                  }}
                />
              </label>
              <button
                type="button"
                onClick={() => {
                  moveStep(index, -1)
                }}
              >
                {`Move step ${number} up`}
              </button>
              <button
                type="button"
                onClick={() => {
                  moveStep(index, 1)
                }}
              >
                {`Move step ${number} down`}
              </button>
              <button
                type="button"
                onClick={() => {
                  patch({ steps: draft.steps.filter((_, i) => i !== index) })
                }}
              >
                {`Remove step ${number}`}
              </button>
            </div>
            {step.zones.length > maxConcurrentZones ? (
              <p className="step__warning" data-testid={`wave-warning-${number}`}>
                {`Runs in waves: ${maxConcurrentZones} zones at a time`}
              </p>
            ) : null}
          </div>
        )
      })}
      <button
        type="button"
        onClick={() => {
          patch({ steps: [...draft.steps, { zones: [], durationSeconds: DEFAULT_STEP_SECONDS }] })
        }}
      >
        Add step
      </button>

      <div data-testid="editor-total">
        Total {formatDuration(totalRuntimeSeconds(draft.steps, maxConcurrentZones))}
      </div>

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
