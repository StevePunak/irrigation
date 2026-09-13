import { render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import ProgramsScreen from './ProgramsScreen'
import * as client from '../api/client'
import { ApiError, type Program } from '../api/types'
import { idleStatus, morningProgram, zoneFixtures } from '../test/fixtures'

const refresh = vi.fn()

const eveningProgram: Program = {
  id: 2,
  name: 'Evening',
  enabled: false,
  dayMode: 'EveryNDays',
  dowMask: 0,
  intervalDays: 3,
  anchorDate: '2026-04-01',
  startTimes: [{ id: 4, minutesAfterMidnight: 1140, timezone: 'America/New_York' }],
  zones: [{ id: 23, zoneId: 8, sequence: 1, durationSeconds: 1200 }],
  nextRunUtc: null,
}

beforeEach(() => {
  vi.spyOn(client, 'getPrograms').mockResolvedValue([morningProgram, eveningProgram])
  vi.spyOn(client, 'getZones').mockResolvedValue(zoneFixtures)
  vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)
  vi.spyOn(client, 'runProgram').mockResolvedValue(undefined)
})

afterEach(() => {
  vi.restoreAllMocks()
  refresh.mockReset()
})

describe('ProgramsScreen', () => {
  it('lists every program with its day rule', async () => {
    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const morning = await screen.findByTestId('program-1')
    expect(within(morning).getByText('Morning')).toBeInTheDocument()
    expect(within(morning).getByTestId('day-rule')).toHaveTextContent('Mon, Wed')

    const evening = screen.getByTestId('program-2')
    expect(within(evening).getByTestId('day-rule')).toHaveTextContent('Every 3 days from 1 Apr 2026')
  })

  it('renders start times as wall-clock labels', async () => {
    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
    const morning = await screen.findByTestId('program-1')
    expect(within(morning).getByTestId('start-times')).toHaveTextContent('6:00 AM')
  })

  it('names the zone of a start time that does not match the controller', async () => {
    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const evening = await screen.findByTestId('program-2')
    expect(within(evening).getByTestId('start-times')).toHaveTextContent('7:00 PM')
    expect(within(evening).getByTestId('start-times')).toHaveTextContent('America/New_York')

    const morning = screen.getByTestId('program-1')
    expect(within(morning).getByTestId('start-times')).not.toHaveTextContent('America/Los_Angeles')
  })

  it('shows the zone sequence with each duration and the computed total', async () => {
    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)

    const morning = await screen.findByTestId('program-1')
    const sequence = within(morning).getByTestId('zone-sequence')

    // zoneIds 7 and 9 are zone numbers 1 and 3.
    expect(sequence).toHaveTextContent('1. Front lawn 10 min')
    expect(sequence).toHaveTextContent('2. Roses 5 min')
    expect(within(morning).getByTestId('total-runtime')).toHaveTextContent('15 min')
  })

  it('renders next run in the controller zone, and a placeholder when absent', async () => {
    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)

    expect(await within(await screen.findByTestId('program-1')).findByTestId('next-run')).toHaveTextContent(
      '6:00 AM',
    )
    expect(within(screen.getByTestId('program-2')).getByTestId('next-run')).toHaveTextContent('—')
  })

  it('runs a program by its program id', async () => {
    const user = userEvent.setup()
    const runProgram = vi.spyOn(client, 'runProgram').mockResolvedValue(undefined)

    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(within(await screen.findByTestId('program-2')).getByRole('button', { name: /run now/i }))

    expect(runProgram).toHaveBeenCalledWith(2)
  })

  it('sends the whole program when the enable toggle flips', async () => {
    const user = userEvent.setup()
    const updateProgram = vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(within(await screen.findByTestId('program-1')).getByRole('switch'))

    expect(updateProgram).toHaveBeenCalledTimes(1)
    const [id, draft] = updateProgram.mock.calls[0]!
    expect(id).toBe(1)
    expect(draft).toEqual({
      name: 'Morning',
      enabled: false,
      dayMode: 'DaysOfWeek',
      dowMask: 0b0000101,
      intervalDays: 0,
      anchorDate: null,
      startTimes: [{ minutesAfterMidnight: 360, timezone: 'America/Los_Angeles' }],
      zones: [
        { zoneId: 7, sequence: 1, durationSeconds: 600 },
        { zoneId: 9, sequence: 2, durationSeconds: 300 },
      ],
    })
  })

  it('reloads the list after a failed toggle so a rejected write cannot look applied', async () => {
    const user = userEvent.setup()
    const getPrograms = vi.spyOn(client, 'getPrograms').mockResolvedValue([morningProgram, eveningProgram])
    vi.spyOn(client, 'updateProgram').mockRejectedValue(new ApiError(500, 'database is locked'))

    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await screen.findByTestId('program-1')
    expect(getPrograms).toHaveBeenCalledTimes(1)

    await user.click(within(screen.getByTestId('program-1')).getByRole('switch'))

    await waitFor(() => {
      expect(getPrograms).toHaveBeenCalledTimes(2)
    })
  })

  it('keeps a failed toggle visible after the reload clears the error state', async () => {
    const user = userEvent.setup()
    vi.spyOn(client, 'getPrograms').mockResolvedValue([morningProgram, eveningProgram])
    vi.spyOn(client, 'updateProgram').mockRejectedValue(new ApiError(500, 'database is locked'))

    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(within(await screen.findByTestId('program-1')).getByRole('switch'))

    expect(await screen.findByRole('alert')).toHaveTextContent(/database is locked/i)
  })

  it('reports the failed toggle when the reload fails too', async () => {
    const user = userEvent.setup()
    vi.spyOn(client, 'getPrograms')
      .mockResolvedValueOnce([morningProgram])
      .mockRejectedValue(new ApiError(503, 'reload failed'))
    vi.spyOn(client, 'updateProgram').mockRejectedValue(new ApiError(500, 'toggle failed'))

    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(within(await screen.findByTestId('program-1')).getByRole('switch'))

    const alert = await screen.findByRole('alert')
    expect(alert).toHaveTextContent(/toggle failed/i)
    expect(alert).not.toHaveTextContent(/reload failed/i)
  })

  it('says so when there are no programs', async () => {
    vi.spyOn(client, 'getPrograms').mockResolvedValue([])
    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
    expect(await screen.findByText(/no programs yet/i)).toBeInTheDocument()
  })

  it('opens the editor on a new program and returns to the list', async () => {
    const user = userEvent.setup()
    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await screen.findByTestId('program-1')

    await user.click(screen.getByRole('button', { name: /new program/i }))
    expect(await screen.findByRole('heading', { name: /new program/i })).toBeInTheDocument()

    await user.click(screen.getByRole('button', { name: /cancel/i }))
    expect(await screen.findByTestId('program-1')).toBeInTheDocument()
  })

  it('reloads the list after the editor saves', async () => {
    const user = userEvent.setup()
    const getPrograms = vi.spyOn(client, 'getPrograms').mockResolvedValue([morningProgram])
    vi.spyOn(client, 'updateProgram').mockResolvedValue(undefined)

    render(<ProgramsScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(within(await screen.findByTestId('program-1')).getByRole('button', { name: /^edit$/i }))
    await user.click(await screen.findByRole('button', { name: /save/i }))

    await waitFor(() => {
      expect(getPrograms).toHaveBeenCalledTimes(2)
    })
  })
})

describe('ProgramsScreen with no status', () => {
  it('marks times it cannot place rather than guessing a zone', async () => {
    render(<ProgramsScreen status={null} polls={0} refresh={refresh} />)

    const morning = await screen.findByTestId('program-1')
    expect(within(morning).getByTestId('next-run')).toHaveTextContent('--')
    expect(within(morning).getByTestId('start-times')).toHaveTextContent('America/Los_Angeles')
  })
})
