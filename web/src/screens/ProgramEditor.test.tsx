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

function renderEditor(program: Parameters<typeof ProgramEditor>[0]['program'], maxConcurrentZones = 2) {
  return render(
    <ProgramEditor
      program={program}
      zones={zoneFixtures}
      controllerZone={LA}
      maxConcurrentZones={maxConcurrentZones}
      onDone={onDone}
      onCancel={onCancel}
    />,
  )
}

/** Adds a step and puts the zone with database id `zoneId` in it. */
async function addStepWithZone(user: ReturnType<typeof userEvent.setup>, stepNumber: number, zoneId: number) {
  await user.click(screen.getByRole('button', { name: 'Add step' }))
  await user.selectOptions(screen.getByLabelText(`Add a zone to step ${stepNumber}`), String(zoneId))
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
    expect(validationError({ ...base, name: '', dowMask: 1, steps: [{ zones: [7], durationSeconds: 60 }] })).toMatch(/name/i)
  })

  it('requires at least one step', () => {
    expect(validationError({ ...base, name: 'X', dowMask: 1, steps: [] })).toMatch(/step/i)
  })

  it('requires a zone in every step', () => {
    expect(
      validationError({ ...base, name: 'X', dowMask: 1, steps: [{ zones: [7], durationSeconds: 60 }, { zones: [], durationSeconds: 60 }] }),
    ).toMatch(/step 2 needs at least one zone/i)
  })

  it('requires at least one start time', () => {
    expect(
      validationError({ ...base, name: 'X', dowMask: 1, startTimes: [], steps: [{ zones: [7], durationSeconds: 60 }] }),
    ).toMatch(/start time/i)
  })

  it('requires at least one weekday in DaysOfWeek mode', () => {
    expect(
      validationError({ ...base, name: 'X', dowMask: 0, steps: [{ zones: [7], durationSeconds: 60 }] }),
    ).toMatch(/day/i)
  })

  it('requires an interval and an anchor in EveryNDays mode', () => {
    const everyN = { ...base, name: 'X', dayMode: 'EveryNDays' as const, steps: [{ zones: [7], durationSeconds: 60 }] }
    expect(validationError({ ...everyN, intervalDays: 0, anchorDate: '2026-04-01' })).toMatch(/interval/i)
    expect(validationError({ ...everyN, intervalDays: 3, anchorDate: null })).toMatch(/date/i)
    expect(validationError({ ...everyN, intervalDays: 3, anchorDate: '2026-04-01' })).toBeNull()
  })

  it('rejects a zero-length zone run', () => {
    expect(
      validationError({ ...base, name: 'X', dowMask: 1, steps: [{ zones: [7], durationSeconds: 0 }] }),
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

    await addStepWithZone(user, 1, 9)
    await user.clear(screen.getByLabelText('Step 1 minutes'))
    await user.type(screen.getByLabelText('Step 1 minutes'), '5')

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
      steps: [{ zones: [9], durationSeconds: 300 }],
    })
    expect(onDone).toHaveBeenCalled()
  })

  it('stores the zone database id in a step', async () => {
    const user = userEvent.setup()
    const createProgram = vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)

    renderEditor(null)
    await user.type(screen.getByLabelText(/program name/i), 'Evening')
    await user.click(screen.getByRole('button', { name: 'Mon' }))
    await user.click(screen.getByRole('button', { name: 'Add step' }))

    // Option values are zone ids; the label the user reads carries the number.
    const picker = screen.getByLabelText('Add a zone to step 1')
    expect(within(picker).getByRole('option', { name: /3 · Roses/ })).toHaveValue('9')

    await user.selectOptions(picker, '9')
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(createProgram).toHaveBeenCalled()
    })
    expect(createProgram.mock.calls[0]![0].steps[0]!.zones[0]).toBe(9)
  })

  it('accepts the last minute of the day', async () => {
    const user = userEvent.setup()
    const createProgram = vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)

    renderEditor(null)
    await user.type(screen.getByLabelText(/program name/i), 'Late')
    await user.click(screen.getByRole('button', { name: 'Mon' }))
    await user.clear(screen.getByLabelText(/start time 1/i))
    await user.type(screen.getByLabelText(/start time 1/i), '23:59')
    await addStepWithZone(user, 1, 7)
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
    await addStepWithZone(user, 1, 7)
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
    expect(screen.getByTestId('step-1')).toHaveTextContent('1 · Front lawn')
    expect(screen.getByLabelText('Step 1 minutes')).toHaveValue(10)
    expect(screen.getByTestId('step-2')).toHaveTextContent('3 · Roses')
    expect(screen.getByLabelText('Step 2 minutes')).toHaveValue(5)
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
    expect(draft.steps).toEqual([
      { zones: [7], durationSeconds: 600 },
      { zones: [9], durationSeconds: 300 },
    ])
  })

  it('saves steps in their moved order', async () => {
    const user = userEvent.setup()
    const updateProgram = vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await user.click(screen.getByRole('button', { name: 'Move step 2 up' }))
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(updateProgram).toHaveBeenCalled()
    })
    expect(updateProgram.mock.calls[0]![1].steps).toEqual([
      { zones: [9], durationSeconds: 300 },
      { zones: [7], durationSeconds: 600 },
    ])
  })

  it('saves the remaining steps in order after removing a middle one', async () => {
    const user = userEvent.setup()
    const updateProgram = vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    renderEditor(morningProgram)
    await addStepWithZone(user, 3, 11)
    await user.click(screen.getByRole('button', { name: 'Remove step 2' }))
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(updateProgram).toHaveBeenCalled()
    })
    expect(updateProgram.mock.calls[0]![1].steps.map((step) => step.zones)).toEqual([[7], [11]])
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

describe('an editor opened before the controller timezone is known', () => {
  it('refuses to save while the controller timezone is unknown', async () => {
    const user = userEvent.setup()
    const createProgram = vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)

    render(
      <ProgramEditor
        program={null}
        zones={zoneFixtures}
        controllerZone=""
        maxConcurrentZones={2}
        onDone={onDone}
        onCancel={onCancel}
      />,
    )
    await user.type(screen.getByLabelText(/program name/i), 'Evening')
    await user.click(screen.getByRole('button', { name: 'Mon' }))
    await addStepWithZone(user, 1, 7)
    await user.click(screen.getByRole('button', { name: /save/i }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/timezone is not known yet/i)
    expect(createProgram).not.toHaveBeenCalled()
  })

  it('saves with the controller timezone once it arrives', async () => {
    const user = userEvent.setup()
    const createProgram = vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)

    const { rerender } = render(
      <ProgramEditor
        program={null}
        zones={zoneFixtures}
        controllerZone=""
        maxConcurrentZones={2}
        onDone={onDone}
        onCancel={onCancel}
      />,
    )
    await user.type(screen.getByLabelText(/program name/i), 'Evening')
    await user.click(screen.getByRole('button', { name: 'Mon' }))
    await addStepWithZone(user, 1, 7)
    rerender(
      <ProgramEditor
        program={null}
        zones={zoneFixtures}
        controllerZone={LA}
        maxConcurrentZones={2}
        onDone={onDone}
        onCancel={onCancel}
      />,
    )
    await user.click(screen.getByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(createProgram).toHaveBeenCalledTimes(1)
    })
    expect(createProgram.mock.calls[0]![0].startTimes).toEqual([{ minutesAfterMidnight: 360, timezone: LA }])
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

describe('steps', () => {
  it('adds zones to a step as chips and removes them again', async () => {
    const user = userEvent.setup()
    renderEditor(null)

    await addStepWithZone(user, 1, 7)
    await user.selectOptions(screen.getByLabelText('Add a zone to step 1'), '9')

    const step = screen.getByTestId('step-1')
    expect(step).toHaveTextContent('1 · Front lawn')
    expect(step).toHaveTextContent('3 · Roses')

    await user.click(screen.getByRole('button', { name: 'Remove 1 · Front lawn from step 1' }))
    expect(screen.queryByRole('button', { name: 'Remove 1 · Front lawn from step 1' })).toBeNull()
  })

  it('offers only zones the step does not already hold', async () => {
    const user = userEvent.setup()
    renderEditor(null)

    await addStepWithZone(user, 1, 7)

    const picker = screen.getByLabelText('Add a zone to step 1')
    expect(within(picker).queryByRole('option', { name: /1 · Front lawn/ })).toBeNull()
    expect(within(picker).getByRole('option', { name: /3 · Roses/ })).toBeInTheDocument()
  })

  it('warns that a step with more zones than the cap runs in waves', async () => {
    const user = userEvent.setup()
    renderEditor(null, 2)

    await addStepWithZone(user, 1, 7)
    await user.selectOptions(screen.getByLabelText('Add a zone to step 1'), '8')
    expect(screen.queryByTestId('wave-warning-1')).toBeNull()

    await user.selectOptions(screen.getByLabelText('Add a zone to step 1'), '9')
    expect(screen.getByTestId('wave-warning-1')).toHaveTextContent(/runs in waves/i)
  })

  it('totals the runtime assuming waves', async () => {
    const user = userEvent.setup()
    renderEditor(null, 2)

    await addStepWithZone(user, 1, 7)
    await user.selectOptions(screen.getByLabelText('Add a zone to step 1'), '8')
    await user.selectOptions(screen.getByLabelText('Add a zone to step 1'), '9')
    await addStepWithZone(user, 2, 10)
    await user.clear(screen.getByLabelText('Step 2 minutes'))
    await user.type(screen.getByLabelText('Step 2 minutes'), '5')

    // Step 1: three zones under a cap of two, 10 min each wave; step 2: 5 min.
    expect(screen.getByTestId('editor-total')).toHaveTextContent('25 min')
  })

  it('refuses to save a step with no zone', async () => {
    const user = userEvent.setup()
    const createProgram = vi.spyOn(client, 'createProgram').mockResolvedValue(undefined)
    renderEditor(null)

    await user.type(screen.getByLabelText(/program name/i), 'Empty step')
    await user.click(screen.getByRole('button', { name: 'Mon' }))
    await user.click(screen.getByRole('button', { name: 'Add step' }))
    await user.click(screen.getByRole('button', { name: /save/i }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/step 1 needs at least one zone/i)
    expect(createProgram).not.toHaveBeenCalled()
  })
})
