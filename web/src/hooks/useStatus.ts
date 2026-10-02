import { useCallback, useEffect, useRef, useState } from 'react'
import { getStatus } from '../api/client'
import type { Status } from '../api/types'

export const RUNNING_POLL_MS = 2000
export const IDLE_POLL_MS = 15000

export interface StatusState {
  status: Status | null
  error: string | null
  stale: boolean
  /** Increments on each successful poll. Identifies which poll a value came from. */
  polls: number
  refresh: () => void
}

function intervalFor(status: Status | null): number {
  const active =
    status !== null && (status.running.length > 0 || status.program !== null || status.queue.length > 0)
  return active ? RUNNING_POLL_MS : IDLE_POLL_MS
}

export function useStatus(): StatusState {
  const [status, setStatus] = useState<Status | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [stale, setStale] = useState(false)
  const [polls, setPolls] = useState(0)

  const timer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const inFlight = useRef(false)
  const refreshQueued = useRef(false)
  const mounted = useRef(true)

  const poll = useCallback(async () => {
    if (inFlight.current) {
      return
    }
    inFlight.current = true
    const afterRefresh = refreshQueued.current
    refreshQueued.current = false

    let next = IDLE_POLL_MS
    try {
      const fresh = await getStatus()
      if (mounted.current) {
        setStatus(fresh)
        setError(null)
        setStale(false)
        setPolls((count) => count + 1)
      }
      // A run or stop request is answered before the daemon acts on it, so the poll a refresh triggers can predate the change.
      next = afterRefresh ? RUNNING_POLL_MS : intervalFor(fresh)
    } catch (caught: unknown) {
      if (mounted.current) {
        setError(caught instanceof Error ? caught.message : String(caught))
        setStale(true)
      }
    } finally {
      inFlight.current = false
    }

    if (mounted.current) {
      if (timer.current !== null) {
        clearTimeout(timer.current)
      }
      timer.current = setTimeout(() => {
        void poll()
      }, refreshQueued.current ? 0 : next)
    }
  }, [])

  useEffect(() => {
    mounted.current = true
    void poll()

    return () => {
      mounted.current = false
      if (timer.current !== null) {
        clearTimeout(timer.current)
        timer.current = null
      }
    }
  }, [poll])

  const refresh = useCallback(() => {
    refreshQueued.current = true
    if (timer.current !== null) {
      clearTimeout(timer.current)
      timer.current = null
    }
    void poll()
  }, [poll])

  return { status, error, stale, polls, refresh }
}
