import { render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import ProgramEditor, { emptyDraft, validationError } from './ProgramEditor'
import * as client from '../api/client'
import { ApiError } from '../api/types'
import { morningProgram, zoneFixtures } from '../test/fixtures'

const onDone = vi.fn()
const onCancel = vi.fn()

const LA = 'America/Los_Angeles'

function renderEditor(program: Parameters<typeof ProgramEditor>[0]['program']) {
  return render(
    <ProgramEditor
      program={program}
      zones={zoneFixtures}
      controllerZone={LA}
      onDone={onDone}
      onCancel={onCancel}
    />,
  )
}

beforeEach(() => {
  vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)
  vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)
  vi.spyOn(client, 'deleteProgram').mockResolvedValue(undefined)
  vi.stubGlobal('confirm', vi.fn(() => true))
})

afterEach(() => {
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
  onDone.mockReset()
  onCancel.mockReset()
})

describe('emptyDraft', () => {
  it('defaults the start time to the controller timezone', () => {
    expect(emptyDraft(LA).startTimes).toEqual([{ minutesAfterMidnight: 360, timezone: LA }])
  })

  it('defaults the day mode to DaysOfWeek', () => {
    expect(emptyDraft(LA).dayMode).toBe('DaysOfWeek')
  })

  it('defaults enabled to true', () => {
    expect(emptyDraft(LA).enabled).toBe(true)
  })
})

describe('validationError', () => {
  const base = emptyDraft(LA)

  it('requires a name', () => {
    expect(validationError({ ...base, name: '', dowMask: 1, zones: [{ zoneId: 7, sequence: 1, durationSeconds: 60 }] })).toMatch(/name/i)
  })

  it('requires at least one zone', () => {
    expect(validationError({ ...base, name: 'X', dowMask: 1, zones: [] })).toMatch(/zone/i)
  })

  it('requires at least one start time', () => {
    expect(
      validationError({ ...base, name: 'X', dowMask: 1, startTimes: [], zones: [{ zoneId: 7, sequence: 1, durationSeconds: 60 }] }),
    ).toMatch(/start time/i)
  })

  it('requires at least one weekday in DaysOfWeek mode', () => {
    expect(
      validationError({ ...base, name: 'X', dowMask: 0, zones: [{ zoneId: 7, sequence: 1, durationSeconds: 60 }] }),
    ).toMatch(/day/i)
  })

  it('requires an interval and an anchor in EveryNDays mode', () => {
    const everyN = { ...base, name: 'X', dayMode: 'EveryNDays' as const, zones: [{ zoneId: 7, sequence: 1, durationSeconds: 60 }] }
    expect(validationError({ ...everyN, intervalDays: 0, anchorDate: '2026-04-01' })).toMatch(/interval/i)
    expect(validationError({ ...everyN, intervalDays: 3, anchorDate: null })).toMatch(/date/i)
    expect(validationError({ ...everyN, intervalDays: 3, anchorDate: '2026-04-01' })).toBeNull()
  })

  it('rejects a zero-length zone run', () => {
    expect(
      validationError({ ...base, name: 'X', dowMask: 1, zones: [{ zoneId: 7, sequence: 1, durationSeconds: 0 }] }),
    ).toMatch(/duration/i)
  })
})

describe('creating a program', () => {
  it('posts the draft the form describes', async () => {
    const user = userEvent.setup()
    const createProgram = vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)

    renderEditor(null)

    await user.type(screen.getByLabelText(/program name/i), 'Evening')
    await user.click(screen.getByRole('button', { name: 'Mon' }))
    await user.click(screen.getByRole('button', { name: 'Wed' }))
    await user.clear(screen.getByLabelText(/start time 1/i))
    await user.type(screen.getByLabelText(/start time 1/i), '19:00')

    await user.click(screen.getByRole('button', { name: /add zone/i }))
    await user.selectOptions(screen.getByLabelText(/zone 1 valve/i), '9')
    await user.clear(screen.getByLabelText(/zone 1 minutes/i))
    await user.type(screen.getByLabelText(/zone 1 minutes/i), '5')

    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(createProgram).toHaveBeenCalledTimes(1)
    })
    expect(createProgram.mock.calls[0]![0]).toEqual({
      name: 'Evening',
      enabled: true,
      dayMode: 'DaysOfWeek',
      dowMask: 0b0000101,
      intervalDays: 0,
      anchorDate: null,
      startTimes: [{ minutesAfterMidnight: 1140, timezone: LA }],
      zones: [{ zoneId: 9, sequence: 1, durationSeconds: 300 }],
    })
    expect(onDone).toHaveBeenCalled()
  })

  it('stores the zone database id, not the zone number', async () => {
    const user = userEvent.setup()
    const createProgram = vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)

    renderEditor(null)
    await user.type(screen.getByLabelText(/program name/i), 'Evening')
    await user.click(screen.getByRole('button', { name: 'Mon' }))
    await user.click(screen.getByRole('button', { name: /add zone/i }))

    // Option values are zone ids; the label the user reads carries the number.
    const picker = screen.getByLabelText(/zone 1 valve/i)
    expect(within(picker).getByRole('option', { name: /3 · Roses/ })).toHaveValue('9')

    await user.selectOptions(picker, '9')
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(createProgram).toHaveBeenCalled()
    })
    expect(createProgram.mock.calls[0]![0].zones[0]!.zoneId).toBe(9)
  })

  it('accepts the last minute of the day', async () => {
    const user = userEvent.setup()
    const createProgram = vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)

    renderEditor(null)
    await user.type(screen.getByLabelText(/program name/i), 'Late')
    await user.click(screen.getByRole('button', { name: 'Mon' }))
    await user.clear(screen.getByLabelText(/start time 1/i))
    await user.type(screen.getByLabelText(/start time 1/i), '23:59')
    await user.click(screen.getByRole('button', { name: /add zone/i }))
    await user.selectOptions(screen.getByLabelText(/zone 1 valve/i), '7')
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(createProgram).toHaveBeenCalled()
    })
    expect(createProgram.mock.calls[0]![0].startTimes[0]!.minutesAfterMidnight).toBe(1439)
  })

  it('refuses to save an invalid draft and says why', async () => {
    const user = userEvent.setup()
    const createProgram = vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)

    renderEditor(null)
    await user.click(screen.getByRole('button', { name: /save/i }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/name/i)
    expect(createProgram).not.toHaveBeenCalled()
  })

  it('surfaces a rejected save and does not close the editor', async () => {
    const user = userEvent.setup()
    vi.spyOn(client, 'createProgram').mockRejectedValue(new ApiError(500, 'database is locked'))

    renderEditor(null)
    await user.type(screen.getByLabelText(/program name/i), 'Evening')
    await user.click(screen.getByRole('button', { name: 'Mon' }))
    await user.click(screen.getByRole('button', { name: /add zone/i }))
    await user.click(screen.getByRole('button', { name: /save/i }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/database is locked/i)
    expect(onDone).not.toHaveBeenCalled()
    expect(onCancel).not.toHaveBeenCalled()
  })
})

describe('editing a program', () => {
  it('loads every field of the existing program', async () => {
    renderEditor(morningProgram)

    expect(screen.getByLabelText(/program name/i)).toHaveValue('Morning')
    expect(screen.getByRole('button', { name: 'Mon' })).toHaveAttribute('aria-pressed', 'true')
    expect(screen.getByRole('button', { name: 'Wed' })).toHaveAttribute('aria-pressed', 'true')
    expect(screen.getByRole('button', { name: 'Tue' })).toHaveAttribute('aria-pressed', 'false')
    expect(screen.getByLabelText(/start time 1/i)).toHaveValue('06:00')
    expect(screen.getByLabelText(/zone 1 valve/i)).toHaveValue('7')
    expect(screen.getByLabelText(/zone 1 minutes/i)).toHaveValue(10)
    expect(screen.getByLabelText(/zone 2 valve/i)).toHaveValue('9')
    expect(screen.getByLabelText(/zone 2 minutes/i)).toHaveValue(5)
  })

  it('puts the full draft under the program id', async () => {
    const user = userEvent.setup()
    const updateProgram = vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.clear(screen.getByLabelText(/program name/i))
    await user.type(screen.getByLabelText(/program name/i), 'Morning revised')
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(updateProgram).toHaveBeenCalledTimes(1)
    })
    const [id, draft] = updateProgram.mock.calls[0]!
    expect(id).toBe(1)
    expect(draft.name).toBe('Morning revised')
    expect(draft.zones).toEqual([
      { zoneId: 7, sequence: 1, durationSeconds: 600 },
      { zoneId: 9, sequence: 2, durationSeconds: 300 },
    ])
  })

  it('save renumbers zone sequence from the final array order after a move', async () => {
    const user = userEvent.setup()
    const updateProgram = vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.click(screen.getByRole('button', { name: /move zone 2 up/i }))
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(updateProgram).toHaveBeenCalled()
    })
    expect(updateProgram.mock.calls[0]![1].zones).toEqual([
      { zoneId: 9, sequence: 1, durationSeconds: 300 },
      { zoneId: 7, sequence: 2, durationSeconds: 600 },
    ])
  })

  it('save renumbers zone sequence from the final array order after removing a middle zone', async () => {
    const user = userEvent.setup()
    const updateProgram = vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.click(screen.getByRole('button', { name: /add zone/i }))
    await user.selectOptions(screen.getByLabelText(/zone 3 valve/i), '11')
    await user.click(screen.getByRole('button', { name: /remove zone 2/i }))
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(updateProgram).toHaveBeenCalled()
    })
    expect(updateProgram.mock.calls[0]![1].zones.map((zone) => [zone.zoneId, zone.sequence])).toEqual([
      [7, 1],
      [11, 2],
    ])
  })

  it('defaults an added start time to the controller timezone', async () => {
    const user = userEvent.setup()
    const updateProgram = vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.click(screen.getByRole('button', { name: /add start time/i }))
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(updateProgram).toHaveBeenCalled()
    })
    const draft = updateProgram.mock.calls[0]![1]
    expect(draft.startTimes).toHaveLength(2)
    expect(draft.startTimes[1]!.timezone).toBe(LA)
  })

  it('shows the surviving start time after removing an earlier one', async () => {
    const user = userEvent.setup()
    renderEditor(morningProgram)

    await user.click(screen.getByRole('button', { name: /add start time/i }))
    const second = screen.getByLabelText(/start time 2/i)
    await user.clear(second)
    await user.type(second, '19:00')

    await user.click(screen.getByRole('button', { name: /remove start time 1/i }))

    expect(screen.getByLabelText(/start time 1/i)).toHaveValue('19:00')
  })
})

describe('blank start times', () => {
  it('refuses to save while a start time is blank', async () => {
    const user = userEvent.setup()
    const updateProgram = vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.clear(screen.getByLabelText(/start time 1/i))
    await user.click(screen.getByRole('button', { name: /save/i }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/start time 1 needs a valid time/i)
    expect(updateProgram).not.toHaveBeenCalled()
  })

  it('names the row that is blank when several start times exist', async () => {
    const user = userEvent.setup()
    const updateProgram = vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.click(screen.getByRole('button', { name: /add start time/i }))
    await user.clear(screen.getByLabelText(/start time 2/i))
    await user.click(screen.getByRole('button', { name: /save/i }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/start time 2 needs a valid time/i)
    expect(updateProgram).not.toHaveBeenCalled()
  })

  it('saves once the blank start time is filled back in', async () => {
    const user = userEvent.setup()
    const updateProgram = vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.clear(screen.getByLabelText(/start time 1/i))
    await user.type(screen.getByLabelText(/start time 1/i), '07:30')
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(updateProgram).toHaveBeenCalledTimes(1)
    })
    expect(updateProgram.mock.calls[0]![1].startTimes[0]!.minutesAfterMidnight).toBe(450)
  })
})

describe('deleting a program', () => {
  it('asks first and then deletes', async () => {
    const user = userEvent.setup()
    const deleteProgram = vi.spyOn(client, 'deleteProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.click(screen.getByRole('button', { name: /delete/i }))

    expect(globalThis.confirm).toHaveBeenCalled()
    await waitFor(() => {
      expect(deleteProgram).toHaveBeenCalledWith(1)
    })
    expect(onDone).toHaveBeenCalled()
  })

  it('does nothing when the confirmation is declined', async () => {
    const user = userEvent.setup()
    vi.stubGlobal('confirm', vi.fn(() => false))
    const deleteProgram = vi.spyOn(client, 'deleteProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.click(screen.getByRole('button', { name: /delete/i }))

    expect(deleteProgram).not.toHaveBeenCalled()
    expect(onDone).not.toHaveBeenCalled()
  })

  it('offers no delete for a program that does not exist yet', () => {
    renderEditor(null)
    expect(screen.queryByRole('button', { name: /delete/i })).toBeNull()
  })

  it('surfaces a rejected delete and does not close the editor', async () => {
    const user = userEvent.setup()
    vi.spyOn(client, 'deleteProgram').mockRejectedValue(new ApiError(500, 'database is locked'))

    renderEditor(morningProgram)
    await user.click(screen.getByRole('button', { name: /delete/i }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/database is locked/i)
    expect(onDone).not.toHaveBeenCalled()
    expect(onCancel).not.toHaveBeenCalled()
  })
})
