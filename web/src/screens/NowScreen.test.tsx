import { act, render, screen, waitFor, within } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import NowScreen from './NowScreen'
import * as client from '../api/client'
import { ApiError } from '../api/types'
import { idleStatus, runningStatus, zoneFixtures } from '../test/fixtures'

const refresh = vi.fn()

beforeEach(() => {
  vi.spyOn(client, 'getZones').mockResolvedValue(zoneFixtures)
  vi.spyOn(client, 'runZone').mockResolvedValue(undefined)
  vi.spyOn(client, 'stopAll').mockResolvedValue(undefined)
  vi.stubGlobal(
    'confirm',
    vi.fn(() => {
      throw new Error('the Now screen must not raise a confirmation dialog')
    }),
  )
})

afterEach(() => {
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
  refresh.mockReset()
})

describe('NowScreen idle', () => {
  it('says nothing is running and shows the next scheduled run in the controller zone', async () => {
    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)

    expect(await screen.findByText(/no zone running/i)).toBeInTheDocument()
    // 2026-09-14T13:00:00Z is 06:00 in America/Los_Angeles.
    expect(screen.getByTestId('next-run')).toHaveTextContent('6:00 AM')
  })

  it('renders one tile per zone, ordered by zone number', async () => {
    const shuffled = [zoneFixtures[2]!, zoneFixtures[0]!, zoneFixtures[1]!]
    vi.spyOn(client, 'getZones').mockResolvedValue(shuffled)

    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)

    const tiles = await screen.findAllByTestId(/^zone-tile-/)
    expect(tiles.map((tile) => tile.getAttribute('data-testid'))).toEqual([
      'zone-tile-1',
      'zone-tile-2',
      'zone-tile-3',
    ])
  })

  it('names each zone', async () => {
    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)
    expect(await screen.findByText('Roses')).toBeInTheDocument()
    expect(screen.getByText('Front lawn')).toBeInTheDocument()
  })
})

describe('NowScreen manual run', () => {
  it('runs the tapped zone by its zone number', async () => {
    const user = userEvent.setup()
    const runZone = vi.spyOn(client, 'runZone').mockResolvedValue(undefined)

    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)

    const roses = await screen.findByTestId('zone-tile-3')
    await user.click(within(roses).getByRole('button', { name: /run/i }))

    expect(runZone).toHaveBeenCalledTimes(1)
    const [zoneArgument, seconds] = runZone.mock.calls[0]!
    expect(zoneArgument.number).toBe(3)
    expect(zoneArgument.id).toBe(9)
    expect(seconds).toBe(600)
  })

  it('runs for the duration the selector shows', async () => {
    const user = userEvent.setup()
    const runZone = vi.spyOn(client, 'runZone').mockResolvedValue(undefined)

    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)

    await user.selectOptions(await screen.findByLabelText(/run for/i), '1800')
    await user.click(within(screen.getByTestId('zone-tile-1')).getByRole('button', { name: /run/i }))

    expect(runZone.mock.calls[0]![1]).toBe(1800)
  })

  it('refreshes the status straight after a run so the poll tightens', async () => {
    const user = userEvent.setup()
    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)

    await user.click(
      within(await screen.findByTestId('zone-tile-1')).getByRole('button', { name: /run/i }),
    )

    await waitFor(() => {
      expect(refresh).toHaveBeenCalled()
    })
  })

  it('will not run a disabled zone', async () => {
    const runZone = vi.spyOn(client, 'runZone').mockResolvedValue(undefined)
    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)

    // zoneFixtures[6] is zone number 7, enabled: false.
    const planters = await screen.findByTestId('zone-tile-7')
    expect(within(planters).getByRole('button', { name: /run/i })).toBeDisabled()
    expect(runZone).not.toHaveBeenCalled()
  })

  it('surfaces a rejected run instead of swallowing it', async () => {
    const user = userEvent.setup()
    vi.spyOn(client, 'runZone').mockRejectedValue(new ApiError(404, 'unknown zone'))

    render(<NowScreen status={idleStatus} polls={1} refresh={refresh} />)
    await user.click(
      within(await screen.findByTestId('zone-tile-1')).getByRole('button', { name: /run/i }),
    )

    expect(await screen.findByRole('alert')).toHaveTextContent(/unknown zone/i)
  })
})

describe('NowScreen while running', () => {
  it('names the running zone and counts down', async () => {
    render(<NowScreen status={runningStatus} polls={1} refresh={refresh} />)

    const banner = await screen.findByTestId('running-banner')
    expect(banner).toHaveTextContent('Roses')
    expect(banner).toHaveTextContent('2:00')
  })

  it('marks the running tile', async () => {
    render(<NowScreen status={runningStatus} polls={1} refresh={refresh} />)
    expect(await screen.findByTestId('zone-tile-3')).toHaveAttribute('data-running', 'true')
    expect(screen.getByTestId('zone-tile-1')).toHaveAttribute('data-running', 'false')
  })

  it('resets the countdown on a new poll that repeats the remaining seconds', async () => {
    vi.useFakeTimers({ shouldAdvanceTime: true })
    try {
      const { rerender } = render(<NowScreen status={runningStatus} polls={1} refresh={refresh} />)

      expect(await screen.findByTestId('running-banner')).toHaveTextContent('2:00')

      await act(async () => {
        await vi.advanceTimersByTimeAsync(3000)
      })
      expect(screen.getByTestId('running-banner')).toHaveTextContent('1:57')

      rerender(<NowScreen status={runningStatus} polls={2} refresh={refresh} />)
      expect(screen.getByTestId('running-banner')).toHaveTextContent('2:00')
    } finally {
      vi.useRealTimers()
    }
  })
})

describe('NowScreen with no status', () => {
  it('does not claim the system is idle when no status has arrived', async () => {
    render(<NowScreen status={null} polls={0} refresh={refresh} />)

    const banner = await screen.findByTestId('running-banner')
    expect(banner).toHaveTextContent(/unknown/i)
    expect(banner).not.toHaveTextContent(/no zone running/i)
    expect(screen.getByTestId('next-run')).not.toHaveTextContent(/none scheduled/i)
  })
})

describe('NowScreen concurrent actions', () => {
  it('does not let a late run failure overwrite a completed stop', async () => {
    const user = userEvent.setup()
    const pending: { reject?: (reason: Error) => void } = {}
    vi.spyOn(client, 'runZone').mockImplementation(
      () =>
        new Promise<void>((_resolve, reject) => {
          pending.reject = reject
        }),
    )
    vi.spyOn(client, 'stopAll').mockResolvedValue(undefined)

    render(<NowScreen status={runningStatus} polls={1} refresh={refresh} />)

    await user.click(
      within(await screen.findByTestId('zone-tile-1')).getByRole('button', { name: /run/i }),
    )
    await user.click(screen.getByRole('button', { name: /stop/i }))

    await waitFor(() => {
      expect(client.stopAll).toHaveBeenCalled()
    })

    await act(async () => {
      pending.reject?.(new Error('zone run failed'))
      await Promise.resolve()
      await Promise.resolve()
    })

    expect(screen.queryByRole('alert')).toBeNull()
  })

  it('re-enables the zone tiles when a superseded action settles', async () => {
    const user = userEvent.setup()
    const pending: { reject?: (reason: Error) => void } = {}
    vi.spyOn(client, 'runZone').mockImplementation(
      () =>
        new Promise<void>((_resolve, reject) => {
          pending.reject = reject
        }),
    )
    // The stop never settles, standing in for a dropped connection.
    vi.spyOn(client, 'stopAll').mockImplementation(() => new Promise<void>(() => {}))

    render(<NowScreen status={runningStatus} polls={1} refresh={refresh} />)

    const tile = await screen.findByTestId('zone-tile-1')
    await user.click(within(tile).getByRole('button', { name: /run/i }))
    await user.click(screen.getByRole('button', { name: /stop/i }))

    await act(async () => {
      pending.reject?.(new Error('zone run failed'))
      await Promise.resolve()
      await Promise.resolve()
    })

    // One request is still outstanding, so the tiles stay disabled.
    expect(within(screen.getByTestId('zone-tile-1')).getByRole('button', { name: /run/i })).toBeDisabled()
  })

  it('re-enables the zone tiles once every request has settled', async () => {
    const user = userEvent.setup()
    vi.spyOn(client, 'runZone').mockRejectedValue(new Error('zone run failed'))

    render(<NowScreen status={runningStatus} polls={1} refresh={refresh} />)

    const tile = await screen.findByTestId('zone-tile-1')
    await user.click(within(tile).getByRole('button', { name: /run/i }))

    await waitFor(() => {
      expect(screen.getByRole('alert')).toHaveTextContent(/zone run failed/i)
    })
    expect(within(screen.getByTestId('zone-tile-1')).getByRole('button', { name: /run/i })).toBeEnabled()
  })

  it('keeps the tiles disabled while an earlier run still hangs', async () => {
    const user = userEvent.setup()
    let resolveStop: (() => void) | null = null
    // The run never settles, standing in for a dropped connection.
    vi.spyOn(client, 'runZone').mockImplementation(() => new Promise<void>(() => {}))
    vi.spyOn(client, 'stopAll').mockImplementation(
      () =>
        new Promise<void>((resolve) => {
          resolveStop = () => {
            resolve()
          }
        }),
    )

    render(<NowScreen status={runningStatus} polls={1} refresh={refresh} />)

    const tile = await screen.findByTestId('zone-tile-1')
    await user.click(within(tile).getByRole('button', { name: /run/i }))
    await user.click(screen.getByRole('button', { name: /stop/i }))

    await act(async () => {
      resolveStop?.()
      await Promise.resolve()
      await Promise.resolve()
    })

    expect(
      within(screen.getByTestId('zone-tile-1')).getByRole('button', { name: /run/i }),
    ).toBeDisabled()
  })
})

describe('StopButton', () => {
  it('stops on a single tap with no dialog in the way', async () => {
    const user = userEvent.setup()
    const stopAll = vi.spyOn(client, 'stopAll').mockResolvedValue(undefined)

    render(<NowScreen status={runningStatus} polls={1} refresh={refresh} />)
    await user.click(await screen.findByRole('button', { name: /stop/i }))

    expect(stopAll).toHaveBeenCalledTimes(1)
    expect(globalThis.confirm).not.toHaveBeenCalled()
  })

  it('stays live while the status poll is failing', async () => {
    const user = userEvent.setup()
    const stopAll = vi.spyOn(client, 'stopAll').mockResolvedValue(undefined)

    render(<NowScreen status={null} polls={0} refresh={refresh} />)

    const stop = await screen.findByRole('button', { name: /stop/i })
    expect(stop).toBeEnabled()

    await user.click(stop)
    expect(stopAll).toHaveBeenCalledTimes(1)
  })

  it('reports a stop the daemon refused', async () => {
    const user = userEvent.setup()
    vi.spyOn(client, 'stopAll').mockRejectedValue(new ApiError(500, 'could not close the bank'))

    render(<NowScreen status={runningStatus} polls={1} refresh={refresh} />)
    await user.click(await screen.findByRole('button', { name: /stop/i }))

    expect(await screen.findByRole('alert')).toHaveTextContent(/could not close the bank/i)
  })
})
