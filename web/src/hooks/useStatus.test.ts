import { act, renderHook } from '@testing-library/react'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import * as client from '../api/client'
import { idleStatus, runningStatus } from '../test/fixtures'
import { IDLE_POLL_MS, RUNNING_POLL_MS, useStatus } from './useStatus'

beforeEach(() => {
  vi.useFakeTimers()
})

afterEach(() => {
  vi.useRealTimers()
  vi.restoreAllMocks()
})

/** Lets the hook's in-flight promise settle while fake timers are installed. */
async function settle() {
  await act(async () => {
    await vi.advanceTimersByTimeAsync(0)
  })
}

describe('useStatus', () => {
  it('polls once immediately', async () => {
    const getStatus = vi.spyOn(client, 'getStatus').mockResolvedValue(idleStatus)
    const { result } = renderHook(() => useStatus())
    await settle()

    expect(getStatus).toHaveBeenCalledTimes(1)
    expect(result.current.status?.timezone).toBe('America/Los_Angeles')
  })

  it('polls every 15 s while idle', async () => {
    const getStatus = vi.spyOn(client, 'getStatus').mockResolvedValue(idleStatus)
    renderHook(() => useStatus())
    await settle()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS - 1)
    })
    expect(getStatus).toHaveBeenCalledTimes(1)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(1)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)
  })

  it('polls every 2 s while a zone is running', async () => {
    const getStatus = vi.spyOn(client, 'getStatus').mockResolvedValue(runningStatus)
    renderHook(() => useStatus())
    await settle()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(3)
  })

  it('tightens the interval as soon as a poll reports a run', async () => {
    const getStatus = vi
      .spyOn(client, 'getStatus')
      .mockResolvedValueOnce(idleStatus)
      .mockResolvedValue(runningStatus)

    renderHook(() => useStatus())
    await settle()

    // The idle poll schedules the next one 15 s out; that one reports a run.
    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)

    // From here the cadence must be 2 s.
    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(3)
  })

  it('relaxes the interval when the run ends', async () => {
    const getStatus = vi
      .spyOn(client, 'getStatus')
      .mockResolvedValueOnce(runningStatus)
      .mockResolvedValue(idleStatus)

    renderHook(() => useStatus())
    await settle()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS - RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(3)
  })

  it('keeps the last good status and marks it stale when a poll fails', async () => {
    vi.spyOn(client, 'getStatus')
      .mockResolvedValueOnce(runningStatus)
      .mockRejectedValue(new Error('Failed to fetch'))

    const { result } = renderHook(() => useStatus())
    await settle()
    expect(result.current.stale).toBe(false)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })

    expect(result.current.status?.runningZone).toBe(3)
    expect(result.current.stale).toBe(true)
    expect(result.current.error).toMatch(/Failed to fetch/)
  })

  it('backs off to the idle interval after a failed poll, even mid-run', async () => {
    const getStatus = vi
      .spyOn(client, 'getStatus')
      .mockResolvedValueOnce(runningStatus)
      .mockRejectedValueOnce(new Error('Failed to fetch'))
      .mockResolvedValue(runningStatus)

    renderHook(() => useStatus())
    await settle()
    expect(getStatus).toHaveBeenCalledTimes(1)

    // The running poll schedules the next at 2 s; that one rejects.
    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)

    // After the failure the cadence must relax to 15 s, so 2 s buys nothing.
    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS - RUNNING_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(3)
  })

  it('clears the error on the next success', async () => {
    vi.spyOn(client, 'getStatus')
      .mockRejectedValueOnce(new Error('Failed to fetch'))
      .mockResolvedValue(idleStatus)

    const { result } = renderHook(() => useStatus())
    await settle()
    expect(result.current.error).not.toBeNull()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS)
    })

    expect(result.current.error).toBeNull()
    expect(result.current.stale).toBe(false)
    expect(result.current.status?.runningZone).toBe(0)
  })

  it('keeps polling after a failure rather than giving up', async () => {
    const getStatus = vi.spyOn(client, 'getStatus').mockRejectedValue(new Error('Failed to fetch'))
    renderHook(() => useStatus())
    await settle()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS * 3)
    })

    expect(getStatus.mock.calls.length).toBeGreaterThanOrEqual(4)
  })

  it('counts only successful polls', async () => {
    vi.spyOn(client, 'getStatus')
      .mockResolvedValueOnce(idleStatus)
      .mockRejectedValueOnce(new Error('Failed to fetch'))
      .mockResolvedValue(idleStatus)

    const { result } = renderHook(() => useStatus())
    await settle()
    expect(result.current.polls).toBe(1)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS)
    })
    expect(result.current.polls).toBe(1)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS)
    })
    expect(result.current.polls).toBe(2)
  })

  it('stops polling once unmounted', async () => {
    const getStatus = vi.spyOn(client, 'getStatus').mockResolvedValue(idleStatus)
    const { unmount } = renderHook(() => useStatus())
    await settle()

    unmount()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS * 4)
    })

    expect(getStatus).toHaveBeenCalledTimes(1)
  })

  it('refresh() polls immediately and reschedules from now', async () => {
    const getStatus = vi.spyOn(client, 'getStatus').mockResolvedValue(idleStatus)
    const { result } = renderHook(() => useStatus())
    await settle()

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS / 2)
    })
    expect(getStatus).toHaveBeenCalledTimes(1)

    await act(async () => {
      result.current.refresh()
      await vi.advanceTimersByTimeAsync(0)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(RUNNING_POLL_MS - 1)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)
  })

  it('never overlaps two requests', async () => {
    let resolveFirst: ((value: typeof idleStatus) => void) | null = null
    const getStatus = vi.spyOn(client, 'getStatus').mockImplementation(
      () =>
        new Promise((resolve) => {
          resolveFirst = resolve
        }),
    )

    renderHook(() => useStatus())
    await settle()
    expect(getStatus).toHaveBeenCalledTimes(1)

    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS * 3)
    })
    expect(getStatus).toHaveBeenCalledTimes(1)

    await act(async () => {
      resolveFirst?.(idleStatus)
      await vi.advanceTimersByTimeAsync(0)
    })
    await act(async () => {
      await vi.advanceTimersByTimeAsync(IDLE_POLL_MS)
    })
    expect(getStatus).toHaveBeenCalledTimes(2)
  })
})
